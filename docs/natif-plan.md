# Plan d'architecture cible : retrait de l'émulation PM4 du chemin de rendu

Branche `natif`. Lecture seule, sources : docs/natif*.md, natif/*, gpu/renderer.cpp, kernel/gpu_cp.cpp, d3d_ombre.cpp.
Objectifs fixés : 60 img/s au 99e centile, GPU <= 4 ms et CPU <= 10 ms par image, 2 images en vol, rendu natif seul.

## 1. Constats (existant)
- Le natif possède déjà : cibles (une image Vulkan par CreateSurface, vrai format/MSAA), clear, resolve (copie/MSAA/blit), états, pipelines, shaders SPIR-V (95/159), sommets/tampons/UP, fil natif + 2 lots (natif/device.cpp, raccord.cpp, etats.cpp, sommets.cpp, tampons.cpp).
- Il emprunte encore à l'émulation : `gpu::RendererTextureView` (raccord.cpp:227/236) = tout le cache de textures (copie de resolve, DXT natif B2, décodage GPU/CPU, surveillance de pages `platform::Watch*`), `RendererContext()` (contexte Vulkan, device.cpp:577), `RendererActive()`, présentation (gpu/present.cpp sur un `TextureInfo` de l'émulation) et la cadence (barrières écrites par le CP à la fin réelle du travail GPU émulé, `Visible()` gpu_cp.cpp:145).
- Écart mesuré : 1001 draws, 609 k indices, 22 resolves, ~590 SetTexture, ~55 VS/75 PS par image (natif.md B1). ~60 draws non vus (Clear dessiné, XUI).

## 2. Décision d'architecture
Tout ce qui sert au rendu passe dans `runtime/src/natif/` ; `gpu/` ne garde que ce qui est du Vulkan générique. Trois principes :
1. **Les données sont figées à l'appel D3D (fil du jeu), jamais lues plus tard.** Constantes (copie sur bits dirty), VB dynamiques/UP, IB, et conversion des textures au Unlock/premier usage sont recopiés dans l'anneau de l'image courante. Conséquence : les barrières D3D n'ont plus à attendre le GPU, ce qui supprime le couplage CP <-> GPU (cf. 3.6).
2. **Le jeu ne voit jamais la mémoire GPU** : pas de write-back des resolves vers la mémoire invitée (à confirmer en N0, cf. risque R2).
3. **Une seule horloge** : 60 Hz synthétique (vblank) + `timeline semaphore` par image ; Swap bloque sur l'image N-2, jamais ailleurs.

## 3. Composants cibles (ce que le natif possède)
| Composant | Fichier | Contenu |
|---|---|---|
| Contexte Vulkan | `natif/contexte.cpp` (extrait de gpu/vk.cpp) | instance, device, files, `queueLock`, cache de pipelines sur disque ; plus d'appel à `RendererInit` |
| Textures | `natif/textures.{h,cpp}` | cache clé = (adresse base, adresse mips, format, dim, tailles, niveaux) -> `VkImage` PC. Conversion au Unlock (crochets 821A86F8, 821A8710, fin de 821CF548, 821A99D0/821BA820 génériques) : BC1/2/3 en l'état (détuilage CPU AVX2 ou compute existant `TexDecodePipeline`), BC5 (DXN), 8888/565/4444/5551, L8/A8, 16F/32F en R16/R32 ; **chaîne de mips complète** (niveaux + queue de mips empaquetée 360), cubes (dim 3, 6 couches), swizzle par la vue. Invalidation par compteur de génération incrémenté dans les crochets Unlock/Lock et DestructResource 821BB548 (éviction) ; plus de `WatchProtect` (mprotect). Mise en ligne : `vkCmdCopyBufferToImage` depuis l'anneau, lots groupés, <= 0,5 ms CPU/image en moyenne |
| Cibles/resolve | `natif/cibles.cpp` (sortir de device.cpp) | registre adresse invitée -> image : une texture destination de Resolve (CreateTexture RENDERTARGET ou 1er resolve) est une image native vivante ; `TextureFor(fetch)` la sert si l'adresse recoupe, sinon le cache ci-dessus. Resolve plein écran suivi d'un clear 0x100/0x200 : **échange d'images** (zéro copie) au lieu de copier ; sinon `vkCmdCopyImage/Resolve`. Mip > 0, profondeur MSAA (passe compute/depth resolve), EndTiling 821B8708 et resolve interne de Present 821B95F0 à crocheter |
| Layouts/barrières | `natif/cibles.cpp` | fin du « tout GENERAL + barrière globale » : layout par usage (ATTACHMENT, SHADER_READ, TRANSFER), `synchronization2`, barrières déduites de l'usage précédent, `loadOp DONT_CARE` après clear 0x100 |
| Présentation | `natif/presentation.cpp` (reprend swapchain/FSR/gamma de gpu/present.cpp, fsr1.cpp) | au Swap 821B7968 : source = image natif de la texture passée à Swap (plus de `TextureInfo`) ; mise à l'échelle/FSR/gamma ; un seul `vkQueuePresent` par Swap |
| Cadence | `natif/cadence.cpp` | 2 lots en vol (déjà), `VkSemaphore` timeline ; Swap attend la valeur N-2 ; thread vblank 60 Hz qui appelle le rappel d'interruption graphique (`VdSetGraphicsInterruptCallback`, kernel/gpu.cpp:121) ; présent FIFO (ou MAILBOX en option) |
| Primitives | `natif/sommets.cpp` | liste de quads (13) -> indices 0,1,2,0,2,3 cachés par nombre de quads (16 bits) ; liste de rectangles (8) -> 3 sommets, 4e = v1+v2-v0 calculé à la copie (CPU, petits nombres) ; UP/indexés UP (existant) ; `BeginVertices/BeginIndexedVertices` appelés en ligne par XUI (822CF798) : B0 à terminer, crochet sur la primitive D3D appelée |
| Shaders | `natif/raccord.cpp` + installeur | 159/159 SPIR-V (corriger les 64 VS XDK dans un fork XenosRecomp, cf. étape N5) ; DXC officiel dans l'installeur (`packaging/shaders/precompile-shaders.sh`) ; pipelines préchauffés depuis une liste de clés enregistrée |
| Capture | `natif/device.cpp` | `WAKFU_NATIF_CAPTURE` devient la seule capture (remplace gpu.cpp `WantCapture`/frontbuffer.cpp pour le rendu) |

### 3.6 Synchronisation avec les barrières D3D que le jeu attend
Le jeu n'attend que : (a) BlockOnFence(dev+15216) au Swap (régulation, 1 image d'avance), (b) fences de Lock/Release/Destruct sur ressources, (c) callbacks WAIT_REG_MEM/INTERRUPT (vblank, InsertCallback). Règles :
- Le CP (mode WAKFU_NATIF=2) traite les paquets EVENT_WRITE_SHD / MEM_WRITE / COND_WRITE / INTERRUPT dans l'ordre du flux **à leur lecture**, sans attendre le GPU (c'est correct car le natif a déjà copié ses données, principe 1).
- Exception unique : la barrière d'image écrite au Swap (dev+15216) est publiée quand le **natif a soumis puis terminé l'image N-1** (valeur timeline) ; c'est elle qui borne l'avance du jeu et donne les 2 images en vol.
- Lecture CPU d'une ressource écrite par le GPU (Lock sur plage de resolve) : N0 mesure si le jeu en fait ; si oui, readback ciblé (copie image -> mémoire invitée à l'Unlock/Lock, pas de write-back systématique).

## 4. Ce qui peut être supprimé
- `runtime/src/gpu/renderer.cpp` (2369 l.), `glsl_translator.*`, `shader_cache.*`, `texture_decode.*` après reprise du détuilage dans natif/textures (garder `tiling.h`), toute la chaîne `Resolve*/Deferred*/ExportPhysicalMemory/SyncPhysicalForCpu`, `RendererNativeTextureFilled`.
- `kernel/gpu_cp.cpp` (482 l.) : supprimer suivi de registres (`g_regs`, `WriteRegister`), `LoadShader`/`Draw`/catalogue de shaders (`CpLoadShaderCatalog*`, `CpPrecompileCatalog`, FNV), statistiques par image, `Visible()`/`SyncGpuWrites`. Garder : lecteur de paquets, `MEM_WRITE`/`EVENT_WRITE_SHD`/`COND_WRITE`/`WAIT_REG_MEM`/`INTERRUPT`, registres scratch, marqueur Swap (kCpSwapOpcode). Cible ~150 l.
- `platform::WatchProtect/WatchGen/WatchSerial` et le mprotect associé (gain de fautes de page pour le fil du jeu), sous réserve que rien d'autre ne les utilise.
- `d3d_ombre.cpp` : les crochets cessent d'appeler l'original pour Draw*/Clear/Resolve (plus de PM4 de rendu, gain CPU du jeu) ; on garde Swap, KickOff/InsertFence (barrières), création/Lock/Unlock. Les bits dirty doivent être vidés par notre crochet (ce que 821B68E8 faisait) : étape à part, avec test.
- `kernel/frontbuffer.cpp` si la capture native la remplace.

## 5. Étapes, ordre, critères de sortie
Mesure commune : scène zone 1 sauvegarde du joueur (3600 images après l'image 4000), + une scène de menu/XUI, + écran-titre/vidéo. Instrumentation N0 : temps d'image (histogramme p50/p99/max), temps GPU par `vkCmdWriteTimestamp`, temps CPU des crochets, compteurs « ignorés ».

- **N0 Mesure et audit (sans changer le rendu)** : timestamps GPU par image, journal p99, compteur de Lock/Unlock sur plages de resolve (R2), compteur de Begin*Vertices, inventaire des consommateurs de `gpu::` depuis natif/. Sortie : tableau de base (p99 actuel, GPU ms, CPU ms) et décision R2.
- **N1 Textures propres** : natif/textures.cpp, mips complets, BC5, cubes, éviction DestructResource ; mode « ombre » qui compare chaque vue native à `RendererTextureView` (octets décodés). Sortie : 0 divergence sur 10 min de jeu, 0 « texture de secours » hors resolves, import <= 0,5 ms CPU/image, textures non-DXT incluses (test unitaire de détuilage par format, sur le modèle de `wakfu_unit_texdecode`). **Fait (branche n1-textures, 20/11)** : critères atteints en zone 1 et menus (docs/natif.md, Textures N1) ; cubes/3D non faits (aucun shader du jeu ne les lit), formats DXN/16F/32F codés mais jamais rencontrés.
- **N2 Cibles et resolve complets** : registre adresse -> image, échange zéro copie, mip > 0, profondeur MSAA, EndTiling, resolve de Present ; layouts et barrières exacts. Sortie : compteurs « resolve non fait »/« cible absente » = 0 ; GPU <= 4 ms p99 sur l'image de la zone 1 (si > 4 ms : mesurer par passe avant de continuer).
- **N3 Présentation, cadence, barrières (CP mode 2 réel)** : natif/presentation + cadence + règle 3.6, sans `RendererInit`. Sortie : 3600 images à 60 Hz, p99 <= 16,7 ms (+0,5), aucun blocage > 33 ms, deux images en vol vérifiées (jamais 3), pas de mémoire/état de l'émulation alloué (`RendererActive()` faux).
- **N4 Primitives et trous** : quads, rectangles, Begin*Vertices, sommets SNORM/SCALED, anisotropie, miroir une fois/demi-chemin, fonction alpha non « >= ». Sortie : « ignorés » = 0 en zone 1 et menus ; capture comparée à l'émulation (écart moyen <= 2/255, aucune région > 8/255 sur plus de 0,1 % de l'image).
- **N5 Shaders XDK (menus, XUI, vidéo)** : corriger XenosRecomp (SIGSEGV `ShaderRecompiler::recompile(AluInstruction)`, déréférencement nul ligne 629, assert retiré en Release) dans un fork versionné avec provenance, plutôt qu'un traducteur maison ; précompilation à l'installation avec DXC officiel ; préchauffage des pipelines. Sortie : 159/159 SPIR-V ; « SPIR-V absent » = 0 ; 30 min de jeu sans image > 25 ms due à une création de pipeline.
- **N6 Bascule par zones** : défaut `WAKFU_NATIF=2` (natif seul) successivement zone de jeu, menus/XUI, écran-titre/vidéo ; émulation gardée derrière un drapeau de repli jusqu'à la fin de N6. Sortie par zone : captures acceptées par le propriétaire + critères N3 tenus. **Fait le 20/11 (branche n6, docs/natif.md « N6 ») : natif seul par défaut partout, repli `WAKFU_NATIF=0` ; critère révisé : visuel hors bord d'1 px et hors ombre de la zone 1 (défaut de l'émulation), p99 hors chargements et hors avis de sauvegarde à 30 img/s.** Mesure initiale : (zone 1 : ombre du personnage absente de l'émulation ; menus : bord d'1 px de l'émulation ; p99 hors zone 1 tenu ni par le natif ni par l'émulation).
- **N7 Suppression** : retirer le code du §4, crochets sans appel de l'original pour Draw/Clear/Resolve, tests unitaires adaptés. Sortie : build Linux + Windows, tous les `wakfu_unit_*` verts, CPU jeu p99 <= 10 ms, ~4 000 lignes en moins, p99 d'image inchangé. **Fait le 20/11 (branche n7, docs/natif.md « N7 ») : ~5 400 lignes en moins hors Xenia, fil du jeu 2,05 ms/image hors BlockOnFence (p99 3 ms), p99 d'image inchangé (16,8 ms, vblank).**
- **N9 Lanceur WakfuRecomp (Windows + Linux)** : (lanceur ImGui unique, Linux en .tar.zst + .desktop + pkexec pacman, désinstallation complète sauf sauvegardes/entrées/lanceur, ménage après installation avec case « garder les fichiers de compilation », anciennes installations proposées à la suppression). Indépendant du rendu. Sortie : installation, jeu, ménage et désinstallation testés de bout en bout sous Linux ici et sous Wine pour Windows ; rien hors du dossier d'installation et des raccourcis créés n'est touché (liste des fichiers avant/après).
- **N8 Réglage** : si GPU > 4 ms ou CPU > 10 ms : enregistrement parallèle, lots de descripteurs, pipelines dynamiques (états dynamiques étendus), transient attachments.

N1, N2 et N4 sont parallélisables (fichiers distincts) ; N3 dépend de N2 pour la présentation du bon cible ; N5 est indépendant (peut démarrer maintenant, goulot du calendrier : bloque menus/XUI/vidéo, donc N6 « menus »).

## 6. Risques
- R1 Code D3D du jeu qui écrit en mémoire texture sans Lock/Unlock crochetable (XDK XUI, D3DX interne, XMV vidéo) : invalidation manquée -> textures périmées. Atténuation : mode « ombre » N1 contre `WatchGen`, puis conserver une vérification de somme (échantillonnée) en debug.
- R2 Lecture CPU de résultats de resolve (screenshots, XUI, vidéo) : si présente, le zéro-write-back casse ; N0 le tranche.
- R3 Régulation du jeu : une barrière publiée trop tôt/tard dérègle la cadence (saccades) ou bloque Swap (blocage dur). Test : injection de latence GPU artificielle (+10 ms) ne doit pas bloquer, juste ralentir.
- R4 Budget GPU : 22 resolves/image à 720p (~80 Mo écrits/image si copies) tient mal sur iGPU ; d'où l'échange zéro copie. Matériel de référence à fixer (non précisé) ; sur Windows l'allocation Vulkan est chère (anneaux, pas d'allocation en cours d'image).
- R5 Hitches de pipeline : combinaisons d'états inconnues à l'installation ; liste de clés enregistrée + `VkPipelineCache` livré, sinon saccade au premier usage (p99 faussé).
- R6 Ne pas appeler l'original (N7) : les fonctions D3D internes lisent leur propre état (dirty, KickOff espace, compteurs) ; on peut désynchroniser le device invité. Faire par fonction, avec capture avant/après.
- R7 Dépendances externes : DXC/XenosRecomp (provenance.md, versions épinglées, licence MIT/NCSA) ; le fork XenosRecomp devient à maintenir.
- R8 Vidéo/XUI très peu mesurés : XMV (décodeur) fonctionnel seulement sur Windows exe final ? À valider hors Linux/lavapipe ; différences de pilote (RADV vs Windows) sur BC3 déjà vues (écart 3/255).
- R9 Rien de tout cela n'a été exécuté en jeu côté natif (docs/natif.md : « raccordé, non mesuré ») : toutes les mesures chiffrées du plan sont à produire en N0.


## Révision du 19/11 (rapport N5)
Les 64 VS en échec de XenosRecomp sont les shaders du décodeur WMV matériel de l'XDK (`dxva_pk`, Shader_Mocomp/Residuals/Detile : calcul par vfetch brut + memexport), pas des shaders XUI. Les 95 shaders traduits couvrent le jeu et XUI (à confirmer par N4).

**Corrigé le 19/11 (essais en jeu) : pas de décodage ffmpeg.** Le lecteur XMV du jeu décode par le CPU (sub_822EA7C8, GetNextFrame) dans trois textures 8 bits Y/U/V (1280×720, 640×360, linéaires, trois jeux tournants) ; les shaders `dxva_pk` ne servent pas. Le PS YUV→RGB (conteneur XEX 82053AF8) et son VS (82053828) sont traduits.
- Écran-titre (MenuStart/MenuLoop.wmv, élément vidéo XUI sub_822DB878) : s'affichait déjà en `WAKFU_NATIF=2` (écart moyen 0,09 à 6/255 avec l'émulation, décalage des particules).
- Cinématiques (Chapter1.wmv après « nouvelle partie », chemin vtable+84 qui dessine lui-même) : noires en natif. Deux causes, corrigées dans `natif/` : le VB du quad est à une adresse physique (0B127000, lue désormais en 0xA0000000+, comme l'émulation) ; ses sommets sont en pixels de la cible avec PA_CL_VTE_CNTL = 0x400 (sans échelle X/Y, relevé à dev+0x294C) : positions ramenées en NDC avant le VS (`PixelsVersNdc`), Vulkan découpant avant le viewport. Même image vidéo en mode 1 et 2 : écart 0,00 (deux paires appariées).
- Son des vidéos : non vérifié (essais sans écoute).
N5 se réduit donc à la confirmation N4 des shaders XUI.
