# Rendu natif (chantier B) — inventaire D3D (B0)

> **Depuis N7 (branche n7) : rendu natif seul, émulation PM4 retirée.** Les sections B1 à N6 décrivent l'état d'alors :
> `WAKFU_NATIF`, `WAKFU_NATIF_CAPTURE`, `WAKFU_NATIF_TEX(_OMBRE)`, `WAKFU_NATIF_TEXTURES`, les lignes `[rendu N]` /
> `[image N]` et les options `echelle_rendu` / `filtrage_anisotrope` n'existent plus (section « N7 » en fin de fichier).

D3D9 Xbox 360 de l'XDK 2.0.20353, lié statiquement, sans symboles. Analyse statique (désassemblage + C++ généré),
détail et preuves dans trois fichiers :

- [Présentation, cibles, resolve, clear, tampon de commandes](natif-b0-cibles.md)
- [États, shaders, constantes, flux de sommets, draws](natif-b0-draws.md)
- [Création des ressources, Lock/Unlock, en-têtes, chemins de remplissage](natif-b0-ressources.md)

## Synthèse (adresses sûres sauf mention)

| Rôle | Adresse |
|---|---|
| CreateDevice (1280x720, A8R8G8B8 / D24S8) | 821BC588 |
| Swap (seul appelant de VdSwap) / Present | 821B7968 / 821B7FF0 ; fin d'image du jeu 821073F8 |
| SetRenderTarget (alias appelé 821B5E20) / SetDepthStencilSurface | 821B57F8 / 821B5B48 |
| Clear (cœur ClearF) / Resolve (cœur) | 821B7090 (821B6F68) / 821BA740 (821B95F0) |
| BeginTiling / EndTiling (probable) | 821B8398 / 821B8708 |
| DrawVertices / DrawIndexedVertices / DrawVerticesUP / DrawIndexedVerticesUP (probable) | 821B1F28 / 821B2340 / 821B1900 / 821B1E88 |
| SetTexture / SetStreamSource / SetIndices / SetVertexDeclaration | 821AA658 / 821B4EB8 / 821B5060 / 821AC8A0 |
| SetVertexShader / SetPixelShader | 821AC6D0 / 821AC510 |
| SetRenderState / SetSamplerState | tables {get, set, défaut} 0x82634148 (101) / 0x82634608 (20), appel `bctrl` en ligne |
| Constantes VS/PS F, B, I | 821AC168/821AC240, 821AC318/821AC378, 821AC3D8/821AC430 |
| CreateTexture (générique, 8 arguments) / CreateSurface | 821AA2D8 / 821AA3F8 |
| CreateVertexBuffer / CreateIndexBuffer | 821BAB68 / 821BAC40 |
| LockRect texture / surface (-> 821A99D0) ; UnlockRect | 821AA140 / 821AA5C8 ; 821A86F8 / 821A8710 |
| Lock / Unlock VB-IB | 821BBB58 / 821BBBA8 ; 821BAC30 / 821BACF0 |
| AddRef / Release / DestructResource | 821BAA88 / 821BBAD8 / 821BB548 |
| D3DXCreateTextureFromFile[InMemory]Ex | 821CFD90 / 821CFD20 |

## Conséquences pour B1-B3
- Device : disposition de `GuestDevice` d'UnleashedRecomp jusqu'à 0x27A0 (dirty 0x00, setters 0x40, fetch 0x480,
  constantes VS 0x780 / PS 0x1780, booléens 0x2780), propre à Wakfu au-delà (cf. natif-b0-draws.md) : structure à part.
- Le jeu écrit des constantes directement dans le device (`stfs` vers dev+0x1780) : lire les constantes au draw
  d'après les bits dirty, pas seulement aux Set*ConstantF.
- Ressources : ne pas remplacer les objets (D3DX, XG et Resolve lisent/écrivent le fetch constant de l'en-tête,
  texture +28..+48) ; crocheter UnlockRect / Unlock VB-IB et la fin de 821CF548 (mips D3DX), appeler l'original puis
  convertir au format PC depuis le fetch constant. Crocheter les fonctions génériques (821A99D0, 821BA820) : certaines
  entrées sont atteintes par `b` terminal.
- SetViewport/SetScissorRect, BlockOnFence, KickOff, InsertFence : internes à D3D (non appelés par le jeu).
- À vérifier en B1 (ombre) : bits de Flags de Resolve, noms des SamplerState >= 40, appels indirects.

## B1 — ombre (runtime/src/d3d_ombre.cpp, `WAKFU_OMBRE=1|2`)
Crochets de comptage (sans effet sur le rendu) sur les 24 fonctions ci-dessus ; fin d'image = Swap 821B7968.
Mesure en jeu (sauvegarde du joueur, zone 1, 50 images échantillonnées après l'image 4000, 18/11) :

| | D3D (ombre) | flux PM4 émulé |
|---|---|---|
| draws / image | 1001 (DrawIndexedVertices ≈ 80 %) | 1064 |
| indices / image | 609 000 | 614 000 |
| resolves / image | 22,0 | 21,9 |
| Clear / effacements | 4 Clear | 22,8 (dont effacements des resolves) |

L'inventaire couvre le rendu du jeu. Écart de ≈ 60 draws : draws émis par D3D lui-même ou par des chemins non
crochetés (hypothèse : Clear dessiné, XUI) ; à identifier en B3 avant la bascule.
Par image : ≈ 590 SetTexture, ≈ 1100 SetStreamSource/SetIndices, ≈ 270 SetVertexDeclaration, ≈ 55 VS / 75 PS.

## B2 (première étape) : textures DXT natives

`WAKFU_NATIF_TEXTURES=1` (désactivé par défaut). Crochets UnlockRect 821A86F8 / 821A8710 (d3d_ombre.cpp) : après
l'original, la constante de l'en-tête (+28) est remise au rendu (`RendererNativeTextureFilled`). Au draw suivant, les
formats 18/19/20 sont détuilés et remis dans l'ordre d'octets sur CPU (`TextureBlocks`) puis copiés dans une image
BC1/BC2/BC3 ; `TextureView` la sert (swizzle par la vue) tant que `WatchGen` de la source est inchangé, sinon chemin
existant. D3DX passe par UnlockRect de surface pour chaque niveau : pas de crochet sur 821CF548 (niveau 0 seul servi,
comme le chemin existant). Sans `textureCompressionBC` : chemin existant (RGBA8). Ligne `[natif N]` : converties, servies.
Test : wakfu_unit_texdecode (écart ≤ 2/255 sur lavapipe, ≤ 3/255 sur RADV : décodeur BC3 matériel).

## B3 (cœur) : rendu natif en parallèle

`WAKFU_NATIF=1` (en parallèle de l'émulation ; défaut depuis N6 : 2, natif seul), capture `WAKFU_NATIF_CAPTURE=<image>,<fichier.png>` : voir
[natif-b3-coeur.md](natif-b3-coeur.md) (cœur), [natif-b3-shaders.md](natif-b3-shaders.md) (interface des shaders),
[natif-b3-sommets.md](natif-b3-sommets.md) (déclarations, tampons). L'émulation PM4 reste l'image affichée.

### État (raccordé, non mesuré en jeu)

`runtime/src/natif/raccord.cpp` remplace les bouchons : le natif dessine dans ses cibles hors écran.

- **Shaders** : `CreateVertexShader` 821AE0A8 / `CreatePixelShader` 821ADEC0 crochetés, empreinte XXH3 du conteneur ;
  module `<empreinte>.spv` lu dans `WAKFU_NATIF_SPIRV` (défaut : `spirv` à côté de l'exécutable, message « dossier SPIR-V introuvable » sinon). Disposition :
  set 0 `Texture2D[16]`, sets 1-2 vides, set 3 `SamplerState[16]`, 24 o de push constants (adresses des constantes
  VS c0-c255, PS c0-c255 et partagées, anneau `bufferDeviceAddress` par lot). Test alpha : constante de
  spécialisation (RB_COLORCONTROL bit 3, référence RB_ALPHA_REF). Demi-pixel : (1/l, −1/h) du viewport. Viewport à
  hauteur positive (Y inversé par les VS).
- **Sommets** : `FromDecl` + `Describe`, emplacements fixes des VS (POSITION0 0, TEXCOORD0-3 4-7, COLOR0 8, …) ;
  emplacements sans donnée = (0, 0, 0, 1) par un flux de pas nul. VB/IB par `BufferFor` / `IndexBufferFor` (décalage
  de SetStreamSource tiré de la constante de lecture 95-s), UP convertis dans l'anneau. Unlock VB/IB 821BAC30 /
  821BACF0 et DestructResource 821BB548 crochetés.
- **Textures (N1, `natif/textures.cpp`)** : résultat d'un resolve natif (`VueResolue`), sinon cache natif : clé
  (base, mips, format + mode d'octets, pas + tuilage, tailles, niveaux) -> `VkImage` avec toute sa chaîne de mips (queue
  empaquetée Xenos d'après xenia `texture_util.cc`, BSD-3), détuilée sur CPU (`tiling.h`) et envoyée par l'anneau de
  l'image (`vkCmdCopyBufferToImage` dans les commandes d'envoi du lot). Formats : DXT1/3/5 et DXN en BC1/2/3/5 tels
  quels, 8888, 8, 8_8, 565/1555/4444 développés en RGBA8 (comme l'émulation), 24_8 en R32F, 16F/32F ; swizzle par la vue.
  Invalidation : génération par adresse de base incrémentée par LockRect 821A99D0 et UnlockRect 821A86F8 / 821A8710 ;
  éviction à DestructResource 821BB548 (destruction Vulkan 2 lots plus tard) et après 600 images sans usage. Texture
  déjà liée dans le lot puis réécrite : nouvelle image (les draws précédents gardent l'ancienne). Cubes, 3D et
  tableaux non gérés (aucun shader du jeu ne lit de cube ni de 3D) : ancien chemin `gpu::RendererTextureView`.
  `WAKFU_NATIF_TEX=0` : ancien chemin pour tout. `WAKFU_NATIF_TEX_OMBRE=1` : chaque envoi comparé au décodage de
  référence (DecodeTexture / TextureBlocks / DecodeDepthTexture, reproduits octet pour octet par le décodage GPU de
  l'émulation), et à chaque usage l'empreinte de la source comparée à celle de l'envoi (écriture sans verrou, R1).
  Journal `[natif tex N]` toutes les 60 images. Échantillonneurs d'après la constante de lecture (adressage, filtres
  min/mag/mip, BASEMAP = niveau 0). Test : `wakfu_unit_natif_textures` (sans GPU).
  Mesure (20/11, mode 2, zone 1, 1500 images, même binaire, `WAKFU_NATIF_TEX=0` puis défaut) : présentation p99
  18,38 -> 18,39 ms, max 21,0 -> 19,3 ; GPU natif 4,22 -> 4,01 ms, CPU du fil natif 3,42 -> 2,85 ms. Ombre sur 8 500
  images (menus + 3 500 en zone 1) : 198 envois comparés, 0 contenu différent, 0 périmée, 0 non gérée ; import
  0,08 ms CPU/image en moyenne (0 en zone 1, pointe 4 ms au chargement d'un décor), 117 textures en cache dont 1 à mips.
- GPU : fonctions Vulkan 1.2 `bufferDeviceAddress`, `runtimeDescriptorArray`, `shaderInt64` activées si présentes.
- Test : `wakfu_unit_natif_raccord <conteneurs> <dossier SPIR-V>` (13 paires VS/PS du jeu, avec et sans test alpha :
  pipeline, liaisons, draw UP ; RADV et couche de validation sans erreur).

### Précompilation des shaders (à refaire si les shaders extraits changent)

```
XS=<XenosRecomp> D=$XS/thirdparty/dxc-bin \
XENOSRECOMP=<XenosRecomp compilé> XENOS_INCLUDE=$XS/XenosRecomp/shader_common.h \
DXC=$D/bin/x64/dxc-linux DXC_LIB=$D/lib/x64 XR_HASH=<xr_hash compilé> \
  packaging/shaders/precompile-shaders.sh <shaders extraits> <sortie SPIR-V>
```

Résultat : 95 SPIR-V, 64 échecs (VS XDK, cf. natif-b3-shaders.md). Lancement :
`WAKFU_NATIF=1 WAKFU_NATIF_SPIRV=<sortie SPIR-V> WAKFU_NATIF_CAPTURE=<image>,<fichier.png>`.

Shaders créés par le jeu à l'exécution (5, D3DX du XEX) : `ShaderCree` dépose le conteneur dans
`<WAKFU_NATIF_SPIRV>/conteneurs` puis, si `XENOSRECOMP`, `XENOS_INCLUDE`, `DXC` (et `DXC_LIB` sous Linux) sont posées,
le traduit sur un fil de fond (mêmes options que le script, un outil à la fois, `.spv` écrit en `.tmp` puis renommé).
Le draw est ignoré (« SPIR-V absent ») jusque-là ; `Module` réessaie les absents dès qu'un SPIR-V est créé. Sans
ces variables, ou en cas d'échec, le lanceur les traduit au lancement suivant.

### Journal `[natif N]` (toutes les 60 images)

draws reçus, rendus (dont avec une texture de secours), ignorés par cause : shader non vu à sa création (créé avant
le crochet ou par un autre chemin), SPIR-V absent (64 VS XDK : vidéo, XUI), GPU sans les fonctions, déclaration
absente, tampon indisponible (VB hors limites, anneau plein), primitive (rectangles 8, quads 13), pipeline (création
refusée), liaison (anneau ou jeux de descripteurs pleins), sans cible.

### Ce qui manque

- Mesure en jeu et comparaison des captures (B4) ; rien n'a été lancé.
- Primitives rectangle et quad ; resolves de mip > 0, profondeur MSAA, EndTiling (cf. natif-b3-coeur.md).
- Repli pour les 64 VS XDK (memexport du décodeur WMV : traducteur maison, ou memexport à ajouter à XenosRecomp et au moteur) ; table `table.tsv` non lue (nom de fichier
  = empreinte suffit).
- Textures (N1) : le cache natif garde ses images 2 lots après éviction ; seules les textures non gérées (cubes, 3D,
  tableaux : aucune en zone 1 ni dans les menus) passent encore par l'émulation. Le natif a jusqu'à 2 images de retard : VB dynamiques et textures lues en mémoire invitée au moment du draw
  natif, pas de l'appel D3D (copie à l'Unlock à faire si les captures divergent).
- Échantillonneurs : miroir une fois et demi-chemin traités en bord, pas d'anisotropie ni de biais de mip ; fonction
  alpha autre que « ≥ » non gérée ; formats de sommets SNORM/SCALED non vérifiés sur le GPU.

## Bascule : natif seul (`WAKFU_NATIF=2`)

La fenêtre présente l'image native (resolve final vers le tampon d'image, recopiée par blit, fil natif) ; WAKFU_CAPTURE
capture cette image (numéro VdSwap = image native + 1). Le CP suit le flux PM4 sans rendre : ni draws, ni resolves, ni
effacements, ni décodages. Scratch et interruptions restent immédiats ; MEM_WRITE / EVENT_WRITE / REG_TO_MEM passent par
`natif::Rappel` et deviennent visibles à la fin GPU du lot natif qui suit le travail envoyé avant eux (le jeu réécrit
sommets et constantes après ses barrières). Lot encore ouvert : soumis seul après 1 ms sans commande. Au plus 2 images en
vol (file natif bornée à 2 échanges, 2 emplacements de lot). Relectures CPU : aucune en zone 1 (ligne `[verrou]` absente
sous l'émulation, fautes 0). Mesure zone 1 (19/11, RX 6700, 1500 images, hôte partagé) : 59,97 img/s, p99 23,3 ms
(émulation seule 59,88 img/s, p99 24,6 ms) ; les images longues viennent du fil du jeu (≈ 10 ms de plus hors
BlockOnFence, aussi sous l'émulation). Natif : GPU 4,1 ms / image (1 lot), CPU du fil natif 3,3 ms (ligne `[natif N] par image`).

## N6 : bascule par zones (20/11) — natif seul par défaut

Méthode d'équivalence : une seule partie en `WAKFU_NATIF=1` avec `WAKFU_CAPTURE=<images>` et
`WAKFU_NATIF_CAPTURE=paires` (image native N écrite en `natif_N.png` à côté de `image_N.png` de l'émulation : même état
du jeu, sans écart de cadence ni de particules), puis `python3 runtime/tests/perf/comparer-natif.py <dossier>`.
Cadence, GPU et CPU : partie en `WAKFU_NATIF=2` avec `WAKFU_TRACE` (événements `N`), lignes `[natif N] par image`.
Parcours : vidéos d'ouverture et écran-titre (vidéos jouées), menus XUI, cinématique, zone 1 (RX 6700, RADV).

| Zone | Écart moyen max | > 8/255 max (sans le bord d'1 px) | img/s | p99 | GPU | CPU natif |
|---|---|---|---|---|---|---|
| Écran-titre / vidéo | 0,061/255 | 0,077 % (0 %) | 46,8 (chargements) ; cinématique 59,4 | 149 ms ; cinématique 26,8 ms | 0,4 ms | 2,6 ms (15,4 avant la copie par mots) |
| Menus / XUI | 0,025/255 | 0,170 % (0,017 %) | 59,1 ; avis de sauvegarde 29,5 | 43 ms ; avis 63 ms | 0,35 ms | 1,8 ms |
| Zone 1 | 0,26/255 | 0,41-0,69 % (0,26-0,54 %) | 60,0 | 16,79-16,81 ms | 3,9-4,1 ms | 2,7-2,8 ms |

- Zone 1 : seule différence, l'ombre ronde sous le personnage, dessinée en natif, absente de l'émulation. Elle vient des
  draws du sol (triangles, 24 576 indices, 12 tuiles ; PS : couleur finale multipliée par l'interpolant r1.w). Écartés :
  textures (`WAKFU_NATIF_TEX=0` et `WAKFU_CPU_TEXTURES=1` : même écart), passe du personnage, taches de lumière (93 sommets),
  rendu en cible 256x256. Cause probable : traduction du VS ou du PS du sol (XenosRecomp contre traducteur GLSL de
  l'émulation) ; quel rendu est juste reste à trancher (référence Xbox absente).
- Zone 1, cause établie (20/11, branche n6-ombre) : l'ombre vient du masque d'ombres 512x512 (EDRAM base 0x5A0) :
  effacé en blanc, puis taches LayerShadow en mélange MIN, lu en tf7 (5 lectures) par CombineLayers*. Les traductions
  des shaders en jeu (HeightMap, LayerShadow, CombineLayersStandard) sont équivalentes ligne à ligne entre XenosRecomp
  et le traducteur GLSL de l'émulation. Masques relus au resolve : natif = blanc avec la tache grise (le Clear blanc
  demandé par le jeu via D3D, intercepté au niveau de l'API) ; émulation = noir sauf une bande blanche de 32 px
  (x 480-512), seul rectangle d'effacement qu'elle exécute pour cette cible ; le MIN y reste noir et CombineLayers ne
  voit plus de tache. Défaut de l'émulation (effacement EDRAM incomplet), pas d'un traducteur : le rendu natif est le bon.
- Menus : écart limité au bord d'1 px (dernière ligne/colonne : noir en natif, 9-10/255 ou gris 235 en émulation, défaut
  de bord de l'émulation) ; à l'intérieur 0,017 % au plus. Échec strict du critère à cause de la référence.
- Cadence hors zone 1 : chargements (écran-titre), écran d'avis de sauvegarde rendu par le jeu à 30 img/s (p50 33,3 ms,
  aussi en émulation), 12 images > 33 ms dans les menus (captures, voir plus bas) : p99 < 16,7 ms non tenu, émulation pire.
- Vidéo : envoi des textures Y/U/V (1,3 Mo/image) 13,4 ms CPU/image par texel ; copie par mots de 32 bits pour les
  textures linéaires sans conversion : 1,5 ms (CPU du fil natif 15,4 -> 2,6 ms ; images vidéo identiques).
- **Décision (20/11, propriétaire) : natif jugé juste, critère révisé.** Visuel mesuré hors bord d'1 px et hors ombre
  de la zone 1 (défaut d'effacement EDRAM de l'émulation) ; p99 mesuré hors chargements et hors écran d'avis de
  sauvegarde (30 img/s voulus par le jeu). Mesure sans capture (une capture par N images bloque une image : les
  « 12 images > 33 ms » des menus étaient les captures `toutes:100`) : menus p99 16,86 ms, une image de 135 ms au
  passage à l'avis de sauvegarde (le jeu attend plusieurs barrières, chargement) ; écran-titre : chargement du titre
  (≈ 40 images de 130 ms, l'émulation bloque aussi à cet endroit) ; début de cinématique : 9 images de 33 à 199 ms.
- **Défaut : `WAKFU_NATIF` absent (ou vide) = 2, natif seul.** Repli explicite : `WAKFU_NATIF=0` émulation seule,
  `WAKFU_NATIF=1` natif en parallèle de l'émulation (comparaison). Le lanceur et `jouer.cmd` ne fixent plus la
  variable : un `WAKFU_NATIF=0` posé par le joueur passe jusqu'au jeu. `runtime/tests/batterie.sh` pose
  `WAKFU_NATIF=0` par défaut (j1-j5 et options vérifient le chemin d'émulation).

## N7 : suppression de l'émulation (20/11, branche n7)

- Retirés : `gpu/renderer`, `glsl_translator`, `shader_cache`, `texture_decode`, `kernel/frontbuffer`, outil
  `wakfu_shaders`, sources Xenia (licence gardée : mise en page des textures adaptée de `texture_util.cc`),
  `platform::Watch*` (mprotect), crochets de verrou des VB/IB, `unit_texdecode`, `unit_watch`. Environ 5 400 lignes
  en moins hors Xenia (18 500 avec).
- CP (`kernel/gpu_cp.cpp`) : barrières (MEM_WRITE, EVENT_WRITE_SHD, REG_TO_MEM, COND_WRITE par `natif::Rappel`),
  WAIT_REG_MEM, scratch, interruptions, marqueur de swap ; DRAW_INDX(_2), IM_LOAD(_IMMEDIATE), SET_CONSTANT(2),
  LOAD_ALU_CONSTANT, SET_SHADER_CONSTANTS lus sans effet ; paquet inconnu signalé une fois.
- Crochets (`d3d_ombre.cpp`) : draws (821B1F28, 821B2340, 821B1900, 821B1E88), ClearF 821B6F68 et Resolve 821BA740
  n'appellent plus l'original ; `natif::SynchroConstantes` vide les bits dirty des constantes (dev+0, dev+8). Swap,
  KickOff/fences, créations, Lock/Unlock, Set*, BeginVertices/EndVertices (XUI) appellent toujours l'original. Le verrou
  générique 821A99D0 ne bloque pas sur les fences (désassemblage) : rien ne change pour les textures écrites par le CPU.
- Contexte Vulkan : `natif::InitialiserVulkan` sur le fil du CP (fenêtre ou `WAKFU_RENDER=1`, sinon pas de rendu).
  Capture : `WAKFU_CAPTURE` (image présentée, numéro de VdSwap) ; `WAKFU_STOP_AT_SWAP` attend le traitement des images
  envoyées (`natif::Vider`). Texture non gérée : texture vide (comptée « de secours »). GPU sans
  bufferDeviceAddress/runtimeDescriptorArray/shaderInt64 ou sans SPIR-V : plus de repli, message dans le journal.
- Équivalence : les entrées de test sont en secondes, donc deux parties n'ont pas le même état du jeu à l'image N
  (références `natif_N` de la partie en mode 1, plus lente, inutilisables image par image). Mesure : 43 captures
  (vidéos, menus, cinématique, zone 1) de N7 contre 3674d14 en natif seul, même parcours : 12 identiques, écarts du même
  ordre qu'entre deux parties de 3674d14 (11 identiques) ; cartes d'écart : particules et texte en cours d'écriture.
- A/B zone 1 (RX 6700, vidéos sautées, images 3000-5290, 3 parties chacun) : 60,00 img/s, p99 16,79-16,81 ms contre
  16,80-16,84 (présentation calée sur le vblank : p99 < 16,7 inatteignable, ni avant ni après), GPU 3,82-3,84 ms
  inchangé, CPU du fil natif 2,58 ms contre 2,63, fil du jeu hors BlockOnFence 2,05 ms contre 2,34 ms par image
  (p99 2,90-3,08 contre 3,17-3,51), mesuré sur les événements P/B/b de `WAKFU_TRACE`.

## N8 : portable (GPU intégré Vega 8), son haché, vidéos lentes (21/11, branche n8-portable)

Retours du joueur (Ryzen 7 5700U, Vega 8, CachyOS, PipeWire) : 40-56 img/s en zone 1 (GPU 15-17 ms), vidéos
parfois à ~6 img/s, son haché partout, pas de `journal.txt` sous Linux sans `console = false`.

- **Son haché et vidéos lentes : une seule cause** (prouvée ici avec `SDL_AUDIODRIVER=disk`, la machine de mesure
  n'ayant pas de périphérique son). Le jeu enregistre 3 clients XAudio (4 en partie) ; le pilote mettait leurs
  trames bout à bout au lieu de les additionner. Avec un périphérique, la file se remplissait 3-4 fois trop vite et
  les tours étaient sautés : moteur son du jeu à 1/3-1/4 de sa vitesse (632 tours/10 s au lieu de 1875), trames
  d'autres clients intercalées (son haché), vidéos calées sur l'horloge du son ralenties : logos de l'image 600 à
  2760 (journal du joueur : 600-2520) au lieu de 600-1500, cinématique du chapitre 1 à 6,1-6,6 img/s (rapport du
  joueur : ~6) au lieu de 25,2. Sans périphérique (machine de dev), chaque tour était fait : invisible avant.
  Correction (`kernel/xam.cpp`) : trames d'un tour additionnées ; file de sortie tenue à 12 trames (64 ms, jusqu'à
  4 tours par réveil) ; rappel SDL à 1024 échantillons (quantum PipeWire ; 256 imposait 5 ms au graphe). Vérifié à
  60 img/s, à 25 img/s (cinématique) et à 8 img/s (lavapipe, zone 1) : 1868-1896 tours/10 s, aucune
  sous-alimentation, file min 8 trames, retards du fil jusqu'à 20 ms absorbés.
- **Bilan `[son]` toutes les 10 s** : tours, trames par client, appels et sous-alimentations du périphérique (ms de
  silence), file min, retard max du fil audio, durée max du rappel du jeu ; xma : décodages, sortie vide sans entrée.
- **GPU** (`natif/device.cpp`) : effacements après resolve limités au rectangle (repris de n10), resolves en bandes
  fusionnés, resolve + effacement d'une cible entière par échange d'images, effacement d'une image entière par passe
  CLEAR en disposition d'attache. Zone 1, RX 6700, 3 parties chacun : 3,88 -> 2,71 ms, CPU du fil natif inchangé
  (2,69 ms), captures : écarts sur les éléments animés seulement, comme entre deux parties avant.
  Répartition dans `[natif N] par image` (passes, passes MSAA, resolves, effacements ; 10 images sur 60) ;
  « effacements » inclut les transitions de disposition de l'effacement rapide : une décompression sur Vega (GFX9)
  y apparaîtrait (prochain journal du joueur) ;
  `WAKFU_PROFIL_GPU=1` par cible, `=2` par draw, `WAKFU_PROFIL_SEQ=N` opérations des images N à N+2.
  Reste ≈ 1,7 ms de passes (draws de sprites mélangés, plein écran) : travail de fragments du jeu lui-même
  (1x1 de ciseaux : 1,7 -> 0,65 ms) ; compression (DCC/HTILE) déjà active sur RDNA2 (`RADV_DEBUG=nodcc,nohiz` :
  +0,2 ms). Non mesuré sur Vega 8 (GFX9 : en GENERAL, RADV ne garde pas la DCC ; l'effacement rapide peut y coûter
  une décompression au retour en GENERAL).
- Option `[video] multi_echantillonnage` : `auto` (défaut) = MSAA sur GPU dédié seulement (décision utilisateur du
  21/11 : coupé sur GPU intégré), `active` / `desactive` = choix du joueur (lanceur, onglet Image). Sans MSAA :
  2,7 -> 2,2 ms sur RX 6700, bords un peu différents. Règle et choix du GPU partagés jeu/lanceur
  (`config::ChoisirGpu`, `config::MsaaActif`, testés dans `unit_config`) ; le lanceur énumère les GPU Vulkan
  (chargeur ouvert à l'exécution) et écrit `[gpu] vu/retenu` dans `journal-lanceur.txt`.

