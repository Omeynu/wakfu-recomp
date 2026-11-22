# Rapport du runtime (`PROMPT-runtime.md`)

## État des jalons

| Jalon | État | Vérification |
|---|---|---|
| J1 — Démarrage sans affichage | **Réussi** (2026-11-14) | `runtime/tests/j1.sh` : le jeu atteint `VdInitializeEngines` sans plantage ni stub en chemin |
| J2 — Noyau HLE, boucle principale | **Réussi** (Lua : 2026-11-16) | `runtime/tests/j2.sh` : 1 200 `VdSwap` (~60 images/s), 59 fichiers `Media` ouverts, aucun stub, plantage ni attente bloquée. scripts `.lua` chargés au lancement d'une partie (2026-11-16) |
| J3 — GPU sans rendu | **Réussi, sauf « menu principal atteint »** (2026-11-15) | `runtime/tests/j3.sh` : 600 images, draws/resolves/shaders suivis, catalogue 72 + 85 shaders, aucun paquet inconnu, capture PNG valide (noire : pas de rendu) |
| J4 — Rendu Vulkan (Windows) | **Partiel** (2026-11-15) | `runtime/tests/j4.sh` (Linux, lavapipe) : écran-titre rendu ; menus jusqu'à « New Game » vérifiés par captures ; build Windows croisé + tests J1-J3 sous Wine. Reste : plantage au lancement d'une partie, profondeur, rendu sur le PC |
| J5 — Audio, vidéo, entrées | À faire | — |
| J6 — Finition | **Installateur Windows en un fichier** (2026-11-16) | `WakfuRecomp.exe` (57 Mo) construit par `packaging/windows/build-installer.sh` ; flux complet sous Wine depuis les paquets STFS : préparation, XenonRecomp (C++ identique à la référence Linux), compilation, J1 réussi avec le `wakfu.exe` produit (voir « J6 ») |

## J1 — ce qui existe

- `runtime/src/main.cpp` : 4 Go d'espace invité (`mmap` NORESERVE), image copiée à `0x82000000`,
  en-tête XEX copié en mémoire invitée, table `PPC_LOOKUP_FUNC` remplie (15 430 entrées), thread
  principal (pile `DEFAULT_STACK_SIZE` = 256 Kio, PCR en r13 avec thread courant en +0x100), appel de `_xstart`.
- Gestionnaire de plantage : adresse invitée fautive, 64 derniers imports, pile hôte
  (`-rdynamic` : les fonctions recompilées apparaissent sous leur nom `sub_<adresse invitée>`).
- `runtime/tools/gen_imports.py` : stubs **faibles** journalisés pour les 179 imports (une
  implémentation réelle les remplace au lien) et table des 12 variables noyau (slots dans `.rdata`).
- `runtime/src/kernel/` : 45 imports implémentés (`runtime/imports.md`) : processus, module XEX
  (`XexGetModuleHandle`, `XexGetModuleSection`, `RtlImageXexHeaderField`), mémoire virtuelle et
  physique (tas à pointeur croissant), TLS, temps, config, chaînes (synchronisation : voir J2).

Séquence observée jusqu'au GPU : `RtlImageXexHeaderField` (tas par défaut : absent) →
`NtAllocateVirtualMemory` → `KeGetCurrentProcessType` → sections critiques →
`XexCheckExecutablePrivilege` → `XGetAVPack` → TLS → `KeQuerySystemTime` →
`MmAllocatePhysicalMemoryEx` → **`VdInitializeEngines`**.

## J2 — ce qui existe (130 imports implémentés sur 179, `runtime/imports.md`)

- **Synchronisation** (`kernel/sync.cpp`) : tous les objets attendables sont des en-têtes dispatcher en
  mémoire invitée (événements, sémaphores, threads) ; un verrou global + une variable de condition ;
  handles `Nt*` et pointeurs `Ke*` partagent le même code d'attente (any/all, délais relatifs/absolus).
  Sections critiques et verrous tournants réellement bloquants.
- **Threads** (`kernel/threads.cpp`) : threads hôtes, création suspendue, `NtSuspendThread` sur soi-même
  réellement bloquant (réservoirs de threads du jeu), `ExTerminateThread` par exception, DPC sur un thread dédié.
- **Fichiers** (`kernel/files.cpp`) : `game:\` → jeu de base, `update:\` → TU (le jeu cherche d'abord
  dans `update:`), autres racines → dossier de données inscriptible ; résolution insensible à la casse ;
  liens symboliques ; lecture/écriture/infos/énumération.
- **XAM** (`kernel/xam.cpp`) : profil local, manette 0 branchée, frappes scriptées (`WAKFU_INPUT`), licence
  complète (pas la version d'essai), XMsg (XGI/XMP) en succès hors ligne, pilote audio : rappels toutes
  les 5,33 ms (trames jetées), décodeur XMA (`kernel/xma.cpp`, FFmpeg xma2 facultatif ; registres kick/lock/clear
  traités par crochets après la bibliothèque XMA du jeu, décodage synchrone au kick).
- **GPU** (`kernel/gpu.cpp`, `gpu_cp.cpp`) : anneau exécuté par un processeur PM4 minimal ; vblank à
  60 Hz sur un thread séparé ; interruptions CP délivrées par CPU (masque du paquet `INTERRUPT`).
- **Diagnostics** : `WAKFU_TRACE=1` (tous les imports, thread, LR), `WAKFU_DUMP_AFTER=N` (pile de chaque
  thread), attentes de plus de 3 s, appels indirects vers une cible inconnue (macro
  `PPC_CALL_INDIRECT_FUNC` redéfinie pour le build du runtime).

### Erreurs trouvées en route (utiles pour la suite)

1. Signature Xbox de `NtAllocateVirtualMemory` sans `ZeroBits` : la mauvaise version corrompait le tas.
2. Ordre des octets GPU : le GPU lit la mémoire en petit-boutiste ; le mode « 8 dans 32 » rend la valeur
   vue par le CPU. Les barrières étaient mal écrites (test unitaire ajouté).
3. Le rappel d'interruption efface le bit du CPU qui l'exécute (PCR+0x10C) : il faut l'appeler pour
   chaque CPU du masque, et le vblank ne doit pas dépendre du processeur de commandes (interblocage).
4. Le rappel audio reçoit l'adresse d'une cellule contenant l'argument (`objet = *r3`), et ne doit pas
   être appelé avant que le client ait fini de s'initialiser.
5. Les threads de travail se suspendent eux-mêmes : `NtSuspendThread` doit bloquer réellement.

### Chargement des scripts Lua

Observé au lancement d'une partie (2026-11-16, voir J4).

## J4 — ce qui existe (partiel)

- **Portabilité** : couche `platform` (mémoire, plantages, fichiers, piles) ; compilation croisée Windows x64
  (Clang + MinGW, `cmake/windows-mingw-clang.cmake`) ; J1-J3 réussis sous Wine (`tests/wine-wakfu.sh`) ;
  guide `docs/build-windows.md` (MSYS2 CLANG64).
- **Vulkan** chargé dynamiquement (`gpu/vk.*`), sans écran ; testé sur lavapipe et sous Wine.
- **Shaders** : analyse du microcode par des fichiers repris de Xenia (BSD, `third_party/xenia`) ; traducteur
  maison microcode -> GLSL -> SPIR-V (`gpu/glsl_translator.*`, glslang) ; tous les shaders chargés à
  l'exécution traduits ; outil `wakfu_shaders` (159/159 analysés, 57/57 pixel shaders du jeu).
- **Rendu** (`gpu/renderer.cpp`, `WAKFU_RENDER=1`) : mémoire invitée importée (`VK_EXT_external_memory_host`,
  4 tranches de 128 Mo), constantes, textures (décodage CPU : 8, 8_8, 8888, 565, 1555, 4444, DXT1/3/5), une
  image par cible EDRAM, mélange, primitives (rectangles approchés), resolves tuilés vers la mémoire invitée,
  effacements ; fin d'image dans le flux GPU (marqueur de swap) ; captures exactes. Aucune erreur de validation.
- **Résultats** : logos, écran-titre, menu principal, choix du mode, écran « Sanctuary / New Game » rendus
  correctement (≈ 14 images/s sur lavapipe).
- **Corrections en route** : 4 instructions piégées (`__builtin_debugtrap` sans avertissement) traduites
  (`vcmpbfp(128)`, `vupkd3d128` type 3, `vpkd3d128` type 3, `blrl`) ; test différentiel 66 cas / 423, 0 échec ;
  printf du noyau (test unitaire) ; XAM : stockage, profil, contenus, XMP ; `NtFreeVirtualMemory` réel.

### Fenêtre et manette (`kernel/window.cpp`, `gpu/present.cpp`, `kernel/pad_map.cpp`)

- **Fenêtre** SDL2 1280x720 redimensionnable : `WAKFU_WINDOW=1`, ou par défaut quand aucune variable de
  test n'est posée (`WAKFU_CAPTURE`, `WAKFU_STOP_AT_SWAP`, `WAKFU_STOP_AT_GPU`, `WAKFU_INPUT`) ;
  `WAKFU_WINDOW=0` la désactive. Avec la fenêtre, le rendu est actif par défaut (`WAKFU_RENDER=0` pour l'éteindre).
  Sans SDL2, sans écran ou sans Vulkan : message `[fenetre]`, le jeu continue comme avant.
- **Présentation** à chaque swap : tampon d'image décodé comme la capture PNG (CPU), copié puis étiré (blit,
  proportions gardées, bandes noires) dans la chaîne d'échange (MAILBOX, sinon IMMEDIATE, sinon FIFO), sur le
  contexte Vulkan du rendu. Fenêtre, événements, rendu et présentation sont sur le thread du processeur de
  commandes (une seule file Vulkan). Fermer la fenêtre arrête le jeu (code 0).
- **Manette** : première manette SDL (branchement à chaud) -> `XamInputGetState` (boutons, gâchettes, sticks)
  et frappes `XamInputGetKeystrokeEx` (boutons, gâchettes, directions des sticks). Clavier de secours (fenêtre
  active, touches par position physique) : flèches = croix, Entrée = A, Échap = B, X, Y, Espace = START,
  Retour arrière/Tab = BACK, Page préc./suiv. = LB/RB, Début/Fin = LT/RT, ZQSD (WASD) = stick gauche,
  IJKL = stick droit, C/V = clic des sticks. `WAKFU_INPUT` reste prioritaire pendant ses appuis.
  Test : `wakfu_unit_pad`.

### Partie lancée (2026-11-16)

- Cause du blocage « New Game » : `NtAllocateVirtualMemory` lisait 0x40000000 (MEM_HEAP) comme des pages de
  16 Mo -> alignement 16 Mo, espace virtuel épuisé, compilateur HLSL du jeu en E_OUTOFMEMORY, init de partie
  avortée, `std::map` nul détruit -> récursion. Corrigé : partie lancée, scripts `.lua` chargés (critère J2).
- Mémoire physique vue aussi en 0xC0000000 (D3D convertit les adresses GPU par 0xC0000000 + phys) ; sous Linux
  allouée par Vulkan (dma-buf) et projetée aux deux vues, car amdgpu refuse l'import d'une mémoire partagée.
- Processeur de commandes : chargements de constantes (`SET_CONSTANT` tous types, `LOAD_ALU_CONSTANT`,
  `SET_SHADER_CONSTANTS`) -> fond vidéo de l'écran-titre correct. `MmMapIoSpace` (contexte XMA).
- GPU AMD RX 6700 XT (RADV) disponible sur la machine de test ; j4 : RADV 94 s, lavapipe 57 s (resolves synchrones).
- **Niveau jouable (2026-11-16)** : `runtime/tests/j5-partie.sh` réussi — navigation jusqu'à « New Game », partie
  créée, 89 scripts `.lua` ouverts, ≈ 5 000 images de jeu rendues (≈ 19 images/s en jeu sur RADV), capture finale
  riche. Corrections en route :
  - prédication par zones (`SET_BIN_MASK/SELECT`, bit 0 des paquets type 3) : sans elle, les segments rejoués
    par zone réécrivaient des barrières périmées et `Present` attendait à l'infini ;
  - resolve sur GPU (calcul : tuilage + mode d'octets écrits directement en mémoire invitée), j4 de 94 s à 18 s ;
  - barrières : toute écriture visible du CPU (EVENT_WRITE_SHD, MEM_WRITE, scratch, interruption) a lieu après la fin
    du travail GPU qui la précède (différée depuis J6, cf. ci-dessous) ; mémoire dma-buf synchronisée pour le CPU
    (`DMA_BUF_IOCTL_SYNC`) après chaque fin de lot ;
  - cache des textures entre images (empreinte de la mémoire source) : de 5,7 à 19 images/s en jeu ;
  - `SET_SHADER_CONSTANTS` = 0x56 (0x5B = `EVENT_WRITE_ZPD`) ; réveil anticipé et auto-suspension bornée (20 ms)
    pour `NtSuspendThread`.
- **Étape 3 (rendu, 2026-11-16)** : profondeur + stencil, MSAA 2X/4X ramené à l'image 1X de même base (effacement
  rapide de D3D), rectangles exacts (shader de géométrie), `PA_CL_VTE_CNTL` ; aucune erreur de validation en jeu.
- **Étape 4 (Windows)** : build croisé avec rendu (glslang compilé pour MinGW, `docs/build-windows.md`), j4 réussi sous
  Wine (winevulkan -> lavapipe). Reste : fenêtre SDL2 et présentation.
- **J6 (amorce)** : `tools-src/wakfu-prepare` extrait le jeu (STFS, ISO/XISO XGD1-3, dossier), applique la TU et
  produit base/tu2/patched (sortie identique aux références ; Linux et Windows sous Wine ; ctest 4/4).
- **J6 — performances (2026-11-16, branche `j6-gpu-async`)** :
  - soumission GPU asynchrone (`gpu/renderer.cpp`) : lots soumis aux barrières et en fin d'image sans attente, 3 en vol
    au plus (anneau de constantes, descripteurs, données d'envoi et fence par lot) ; les écritures du CP visibles du
    CPU (EVENT_WRITE_SHD, MEM_WRITE, REG_TO_MEM, scratch, INTERRUPT) et la fin d'image (présentation, capture, arrêt)
    sont rangées avec le dernier lot et appliquées à sa fin réelle, dans l'ordre (`RendererDefer`) ; le thread du CP
    attend les fences (bloquant) au lieu de dormir ; `WAIT_REG_MEM` non satisfait termine d'abord le travail en vol,
    puis attend en cédant la main (200 essais) puis par pas de 100 µs (au lieu de 1 ms) ;
  - aussi : plage exacte des textures pour le test de chevauchement avec les resolves en vol, SPIR-V non recopié à
    chaque draw ; compteurs « lots N (attentes N) » dans les lignes `[rendu N]`.
  - mesure (RADV, navigation de j5-partie.sh, img/s sur les images de jeu [g0+60, g0+1860], g0 = première image à
    ≥ 100 draws) : avant 13,1 / 14,3 / 15,6 / 16,3 / 16,4 (médiane 15,6) ; après 17,3 / 17,8 / 17,8 (médiane 17,8),
    soit ≈ +15 % ; machine partagée avec d'autres builds (bruit). Captures en jeu (image 6000) identiques au pixel près.
  - profil après (CP, en jeu) : ≈ 97 % occupé ; draws ≈ 75 % dont textures ≈ 35 % (empreinte de ≈ 47 Mo de textures
    par image ≈ 13 %, décodage CPU d'une cible 1280x720 resolvée presque à chaque image) et attentes de chevauchement
    ≈ 20 % (≈ 5 par image : texture lue juste après son resolve -> attente du GPU puis décodage CPU). Pistes : copie
    GPU des textures issues d'un resolve (sans relecture CPU), suivi des écritures (protection de pages) au lieu des
    empreintes ; verrou noyau unique (`g_kernelLock` + `notify_all`) ; options XenonRecomp/-O3 non essayées (exigent
    de régénérer `out/ppc`).
- **Performances (2026-11-17, PROMPT-fin étape 4)** — mesure : RADV (RX 6700 XT), build Release, navigation de
  j5-partie.sh, img/s sur les images de jeu [g0+60, g0+1860] (g0 = première image ≥ 100 draws), 5 répétitions, machine
  au repos (seul le jeu tourne) :
  - référence après le correctif du cache (étape 1, empreinte à chaque usage) : 8,77 / 8,78 / 8,82 / 8,84 / 8,93
    (médiane 8,82) — le 17,8 du 16/11 venait en partie du cache fautif ;
  - textures 8_8_8_8 issues d'un resolve décodées sur GPU (calcul lisant la mémoire invitée : tuilage, mode d'octets,
    swizzle comme DecodeTexture ; plus d'empreinte, d'attente ni de décodage CPU) : médiane 13,88 (13,82-13,94) ;
  - idem pour la profondeur 24_8 (formats 22/23, image RGBA32F, quotient arrondi comme sur CPU) : médiane 14,88
    (14,85-14,90), soit +69 % sur la référence. `WAKFU_CPU_TEXTURES=1` revient au décodage CPU ;
  - limiteur (échantillons gdb) : le thread principal attend activement la barrière D3D (`sub_821BD620`) -> chaîne
    CP/GPU ; `[rendu N]` ventile les attentes bloquantes (chevauchement texture CPU / attente du CP / lots en vol) :
    9 -> 4 par image (restent les `WAIT_REG_MEM` du CP sur ses copies de registres scratch 0x8002/0x8006).
    Essai de satisfaire ces attentes par les écritures du CP en attente : 4 -> 1 attente mais aucun gain mesuré
    (14,90 sur 3 mesures valides, 2 parties arrivées en jeu trop tard) -> retiré ;
  - verrou noyau unique (`g_kernelLock`) : non modifié, aucun thread n'y est limité (un seul thread saturé, en
    attente de barrière GPU) ;
  - rendu : capture j4 avec décodage GPU identique au pixel près à une passe CPU ; deux passes CPU diffèrent entre
    elles (animation des menus selon le rythme), d'où pas de comparaison déterministe en jeu.
- **Options du joueur, 1er lot (2026-11-17, étape 5)** : `config.toml` dans le dossier des données (toml++ de
  XenonRecomp), créé au premier lancement avec des défauts identiques au comportement d'avant, documenté dans
  `docs/options.md` : taille et mode de fenêtre, vsync, limite et compteur d'images, choix du GPU par son nom,
  volume général, vibrations (`XamInputSetState` -> SDL), entrée en arrière-plan, logos sautés. Tests :
  `runtime/tests/options.sh` (fenêtre sous Xvfb : 800x600, FIFO, 300 images en 31 s à 10 img/s) et `wakfu_unit_config`.
- **Gel du lecteur vidéo XUI corrigé (J5)** : `KeTimeStampBundle` n'était jamais mis à jour ; `GetTickCount` (son
  champ +0x10) restait à 0 et un minuteur XUI d'intervalle non nul (l'élément vidéo en arme un de 25 à 38 ms quand
  l'image suivante n'est pas due) ne se déclenchait plus. Mis à jour toutes les millisecondes (temps d'interruption,
  heure système, TickCount) : 10 lancements sur 10 avec vidéos (deux vidéos finies, menu atteint), contre 4 sur 6
  avant. `WAKFU_SKIP_VIDEOS=1` reste utile pour raccourcir les tests.
- **Sauvegardes (J5)** : réglages de profil (`XamUserWriteProfileSettings` / `Read`) conservés dans
  `<données>/profile/<id>.bin` (octet de type puis valeur ; le jeu laisse le type à 0 : déduit de l'identifiant ;
  écriture par fichier temporaire + renommage) ; `runtime/tests/j5-sauvegarde.sh` (volume de la musique baissé,
  relu et réécrit à l'identique au lancement suivant) et `wakfu_unit_profile`. Contenus (`XamContentCreateEx`) :
  `save:` -> `<données>/content/<nom>` (déjà en place, journal `[contenu]` à chaque ouverture). La partie
  (`save:\savegame.txt`, contenu « Single-Initiate ») n'est écrite qu'aux points de contrôle Lua
  (`Gameplay.SaveCheckPoint`), non atteints : voir Limites.
- `WAKFU_INPUT` accepte une durée (`40:A/0.5`) et le stick gauche (`LUP LDOWN LLEFT LRIGHT`).
- `0x8225B0D0` (avalée par `sub_8225AF48`, appelée par pointeur) : borne corrigée dans `config/wakfu.toml`, générée
  par XenonRecomp depuis la régénération du 2026-11-17 (version manuelle retirée de `main.cpp`).
- **Zone 1 débloquée (2026-11-17, PROMPT-fin étape 1)** : deux causes à « l'image figée après l'introduction » :
  - base de temps : XenonRecomp traduisait `mftb` par `__rdtsc()` brut (≈ 2,9 GHz ici) alors que
    `KeQueryPerformanceFrequency` annonce 50 MHz. XenonRecomp patché (`mftb` -> `PPC_QUERY_TIMEBASE()`, défini par le
    runtime dans `indirect.h` : `WkQueryTimeBase`, horloge stable à 50 MHz) ; `out/ppc` régénéré (étape 3), le crochet
    provisoire sur `QueryPerformanceCounter` (`sub_821A2798`) est retiré. Le script 10100.lua va jusqu'au bout (`WaitTime(7)`,
    dialogue 82) ;
  - cache des textures : une texture déjà décodée dans l'image était réutilisée sans revérifier la mémoire ; le jeu
    remplit plusieurs fois par image la même zone (mêmes adresse, taille, format), d'où le décor de l'écran de chapitre
    affiché à la place de la forêt (Nora invisible, image fixe). L'empreinte est maintenant recalculée à chaque usage.
    Coût : j5-partie de ≈ 300 s à ≈ 500 s (empreinte CPU par usage) -> à reprendre avec la copie GPU des textures
    issues d'un resolve (étape 4).
  - diagnostic : journal du jeu (`WAKFU_GAMELOG=1`) avec arguments formatés ; minuteur des scripts Lua =
    `sub_8216EAC0(script, &dt)` (+40 attente, +44 écoulé, +48 en attente) ; `j5-partie.sh` déplace Nora au stick et
    échoue si les captures 6000, 6600 et 7200 sont identiques.
- **Sauvegarde de partie (2026-11-17, étape 2)** : `j5-sauvegarde.sh` étendu. Le premier vrai point de contrôle (fosse de
  l'oracle, 10114.lua) n'est pas atteignable par une navigation scriptée à l'aveugle : le test fournit une TU de test
  (copie de `tu2` + `Media/Scripts/10100.lua` terminé par `Gameplay.SaveCheckPoint()` ; le jeu lit `update:` avant
  `game:`). Résultat : `save:\savegame.txt` (64 octets) écrit dans `<données>/content/Single-Initiate`, relu au lancement
  suivant (« result read savegame 0 »), menu « Continue Game » avec le point de contrôle, partie rechargée.
  `RtlTimeToTimeFields` / `RtlTimeFieldsToTime` implémentés (`kernel/time_fields.h`, test `wakfu_unit_time`) : la date
  de la sauvegarde s'affichait « 0/00/00 00:00 » (heure UTC, le fuseau de la console n'est pas appliqué).

### Performances, premier lot (2026-11-17, branche `perf` fusionnée : 63382d0)

Mesure : sauvegarde du joueur, zone 1 (≈ 1 400 draws/image), `runtime/tests/perf/bench-joueur.py`, RADV RX 6700 XT.

| Version | img/s |
|---|---|
| avant XXH3 | 6,1 |
| `main` (XXH3) | 11,0-11,4 |
| + présentation GPU, passe ouverte, constantes conditionnelles (fe999a1) | 13,2 |
| + décodage GPU de tous les formats | 17,6-17,8 |
| + C++ XenonRecomp optimisé + caches shaders/pipelines (exe du 17/11) | 17,5-17,6 (gain du C++ non mesuré par paires) |

- **C++ optimisé** (8 options XenonRecomp, `docs/methode.md` §10) : il se bloquait sur la sauvegarde (thread XUI et
  thread audio dans `RtlEnterCriticalSection`). Cause : les funclets `__finally` de MSVC lisent les registres de leur
  parent ; en variables locales (à 0) ils appelaient `RtlLeaveCriticalSection(0)`. Corrigé sans retirer l'option :
  `registers_in_context` (patch XenonRecomp), 167 fonctions générées sur `ctx`, aides `__save*`/`__rest*` comprises
  (`scripts/find_context_functions.py`). Diagnostic ajouté : `[diag] section critique … attendue > 10 s`.
- **Décodage GPU** : régression sur l'écran-titre sous RADV (plans Y/U/V k_8 linéaires de la vidéo rayés, j4) ;
  décodage identique au CPU (test étendu aux tailles réelles), correct sous lavapipe, copie au moment du CP sans effet :
  cause non élucidée, k_8 linéaire repasse par le CPU. Toutes les textures linéaires sur CPU coûteraient 21 % en zone 1
  (≈ 330 atlas RGBA 512x512 par image, empreinte à chaque usage).
- Précompilation : 74 shaders du catalogue, dont 15 en échec (format de sommet non géré ; non utilisés en zone 1).
- Limiteur restant : voir lot 3.

### Performances, lot 3 (2026-11-18)

| Version | img/s |
|---|---|
| exe du 17/11 | 17,6 |
| + repos du CP 500 -> 20 µs, barrière en tête de lot (cdb9005) | 20,3-20,9 |
| + décodages GPU de textures réutilisés dans l'image (e3a4ee9) | 22,6-23,2 (4 mesures) |
| + indices lus en boucle directe (6dc7ec1, `ReadIndices`) | 24,1 (2 paires, +4 %) |
| + textures décodées sur GPU réutilisées d'une image à l'autre (surveillance des écritures) | 28,9-30,1 |
| + indices lus par le vertex shader (b8250b6) | 32,1 |
| + interruptions et registres scratch du CP immédiats (9032f6b) | 42,8-49,5 |
| + parcours des pages évité (WatchSerial), descripteurs réutilisés (d699f96) | 46,6-54,3 |

Mesures du 18/11 après-midi bruitées : charge de l'hôte 6-10 (autres machines virtuelles), écarts de 5-15 img/s entre
deux mesures d'un même binaire ; seules les comparaisons alternées comptent.

- **Surveillance des écritures** (`platform::Watch*`, comme Xenia) : les pages de la mémoire physique d'où une texture
  est décodée sur GPU passent en lecture seule ; la première écriture (code invité ou hôte) lève une faute qui rend la
  page inscriptible et augmente son numéro d'écriture ; les resolves l'augmentent sans faute ; NtReadFile rend sa plage
  inscriptible avant `pread` (sinon EFAULT). Décodages 116 -> 12-15 par image, GPU 18 -> 10-12 ms. Test : `unit_watch`.
- **Plafond à 30 img/s = vblank** : la trace (`WAKFU_TRACE`, outil `trace-agg.py`) montre que le
  CP attend la synchro verticale (WAIT_REG_MEM sur 0x8004, ≤ 16,7 ms) avant de basculer l'image, comme la console :
  au-delà de 16,7 ms de travail, l'image prend 33,3 ms. Il faut < 16,7 ms de travail par image pour 60 img/s : jeu
  ≈ 3,6 ms, enregistrement du CP ≈ 21 ms (≈ 850 000 indices et ≈ 1 400 draws par image). 0x8000 = rappel D3D
  (InsertCallback, fonction 821B72E0 de cadence) par interruption.
- **Indices** : lus par le vertex shader dans la mémoire invitée (constantes poussées `indexByte`, `indexMode` ;
  quadrilatères par table de coins), draws non indexés : plus de conversion ni de copie sur CPU (≈ 850 000 indices par
  image). CP 21 -> 10,6 ms d'enregistrement par image.
- **Rappels D3D sans vidange** : InsertCallback (scratch0 = 4, interruption, attente de scratch0 = 0 posé par le
  gestionnaire) ; l'interruption et les registres scratch étaient différés à la fin réelle du GPU, d'où une vidange par
  image. Désormais immédiats ; les barrières (MEM_WRITE, EVENT_WRITE) restent différées (sinon rayures, cf. j4).
- **Où en est la cadence** (trace) : le CP reçoit les commandes au fil de l'image ; il doit atteindre l'attente du vblank
  avant le vblank suivant (≈ 12 ms après la présentation) : réussi -> 16,7 ms, raté -> 33,3 ms. Les images ratées
  viennent des images lourdes et de la charge de l'hôte. Sous Windows, pas de `SyncPhysicalForCpu` (≈ 20 % du CP ici).
- **Régression corrigée** : vidéo de l'écran-titre en blocs depuis le 17/11 (exes du 18/11 05 h 50 et 09 h compris) ;
  cause : optimisations XenonRecomp, désactivées (`docs/methode.md` §10). j4 contrôle désormais ces blocs.

- **Limiteur restant** : la chaîne CP -> GPU est en série. Environ 9 fois par image, le CP exécute un `WAIT_REG_MEM`
  sur une valeur que le GPU écrit à la fin de son travail : phys 0x8000 == 4 (vidange, 71 %) et 0x8004 == 1/0
  (bascule, 29 %). Il attend donc la fin réelle du GPU (≈ 19-20 ms/image) avant d'enregistrer la suite. Le thread du
  jeu, lui, attend sa barrière D3D (`BlockOnFence`) ; sa logique coûte peu.
- Essais écartés (mesures par paires) : satisfaire ces attentes dans le flux (rayures j4) ; ne plus attendre 0x8000
  (aucun gain : l'attente 0x8004 qui suit vide le GPU) ; sauter les deux (le jeu n'atteint plus la partie) ; retirer
  les lots un par un jusqu'à la condition (22,9 contre 23,0 : l'attente porte toujours sur le dernier lot).
- Profil fin (build `-g`, 100 échantillons du thread du CP) : la cadence suit le temps d'enregistrement du CP (gain des
  indices). Écartés aussi : barrières complètes en double supprimées (gain nul) ; attente WAIT_REG_MEM par pas de 20 µs
  au lieu de 100 (gain nul). 0x8000/0x8004 sont les copies des registres scratch 0/1 du CP (base 0x8000) : les
  satisfaire quand une écriture différée du CP les remplit fait tomber les attentes de 9 à 1 par image mais ne rapporte
  que +2 % (24,6-24,7) ; non retenu (synchronisation, risque de rayures j4). Patch gardé hors dépôt.
- Linux seulement : `SyncPhysicalForCpu` (synchro dma-buf de toute la mémoire à chaque lot retiré) ≈ 10 % du CP ;
  absent du build Windows.
- Lot 4 (cadence débloquée), étude : la constante 1/60 (0x8201155C) n'est référencée qu'une fois dans le code
  (`sub_82187E70`, initialisation, adresse passée à une méthode virtuelle). Un point d'arrêt matériel en lecture ne
  l'a vue lue que par le thread audio, pas par la boucle du jeu : pas de preuve qu'elle règle le pas de la logique.
  À faire : chronométrage scripté (trajet, `WaitTime`) avant toute modification.
- Pistes : réduire le temps GPU par image (barrières `ALL_COMMANDS` autour de chaque décodage, passes coupées) et le
  temps d'enregistrement du CP (≈ 40 % d'un cœur). La cadence reste sous 60 img/s, donc le jeu tourne au ralenti
  (pas fixe 1/60 par image).

## J3 — ce qui existe

- Processeur PM4 (`kernel/gpu_cp.cpp`) : registres (type 0/1, `SET_CONSTANT`, `REG_RMW`), tampons
  indirects, barrières, `WAIT_REG_MEM`/`COND_WRITE`, interruptions par CPU, chargement des shaders
  (`IM_LOAD`, `IM_LOAD_IMMEDIATE`), draws (`DRAW_INDX`, `DRAW_INDX_2` : type de primitive, indices),
  resolves (draw avec `RB_MODECONTROL` en mode copie, destination `RB_COPY_DEST_*`), paquets inconnus comptés.
- Shaders reliés au catalogue par hachage du microcode : 72 shaders distincts du jeu (74 fichiers, 2 doublons)
  et 85 conteneurs intégrés au XEX (XUI, vidéo, D3DX).
- Statistiques par image (`[image n]` toutes les 60 images) et capture du tampon d'image de `VdSwap`
  (`WAKFU_CAPTURE="60,300"` ou `toutes:N`) : constante de texture, détuilage 2D Xenos, PNG sans dépendance.

Observé dans les 600 premières images : 0 à 6 draws par image (triangles, éventails, rectangles), un
resolve par image, 9 shaders : 3 du XEX, 6 hors catalogue (vraisemblablement générés par D3D pour les
effacements et resolves). Aucun shader de `Media/Shaders` encore : le jeu n'a pas quitté sa séquence
d'ouverture ou ses menus XUI. Le critère « menu principal atteint » et le critère Lua du J2 demandent
de voir l'écran : ils passent au J4. La capture est noire tant que les draws et resolves ne sont pas
exécutés (EDRAM non émulée).

## J6 — installateur Windows (`tools-src/wakfu-installer`, `packaging/windows`)

- Un exécutable : amorce C++ (zstd, Win32) + archive zstd ajoutée en fin de fichier (format `pack.py`, chemins
  validés), extraite une fois par version dans `%LOCALAPPDATA%\WakfuRecomp\outils-<id>`. Contenu (372 Mo
  décompressés) : llvm-mingw 20250114 (LLVM 19.1.7, réduit à x86_64), CMake 3.31.6, ninja 1.12.1,
  `XenonRecomp.exe` patché et `wakfu-prepare.exe` (compilés croisés, statiques), sources suivies de `runtime/`,
  `check/`, `config/`, glslang 15.1.0, SDL2 2.32.10, FFmpeg 7.1.2 (xma2 seul, LGPL), Vulkan-Headers 1.4.309.
  Aucun fichier du jeu, aucun C++ généré.
- Chez l'utilisateur : `wakfu-prepare` → XenonRecomp → stubs d'imports (portage C++ de `gen_imports.py`,
  sorties identiques) → CMake/ninja/Clang → `<D>\wakfu.exe` + `jouer.cmd`. Reprise des étapes faites ;
  sans argument ensuite, lance le jeu.
- Reproductible : versions et SHA-256 dans `deps.lock`, commit XenonRecomp de `tools.lock`, PE sans date
  (`--no-insert-timestamp`) : deux constructions donnent le même `WakfuRecomp.exe`.
- Vérifié sous Wine 10 : extraction, préparation depuis `game/raw` (SHA-1 de l'image attendu), C++ généré
  identique à `out/ppc`, stubs identiques à `runtime-gen`, `wakfu.exe` (35 Mo ; SDL2, FFmpeg, rendu), puis J1 et
  J4 partiel réussis avec cet exe (J4 sous lavapipe : 88 draws, 0 ignoré, 18 resolves, 1 217 couleurs).
  Durées (CT 8 cœurs, Wine) : extraction des outils + préparation + XenonRecomp ≈ 30 s, configuration et
  compilation complète (104 étapes) 2 min 15 s. Disque : outils 372 Mo, jeu préparé 820 Mo, C++ généré 93 Mo, build 124 Mo (≈ 1,5 Go).
  Piège Wine : CMake lancé avec un dossier courant sur `Z:` corrompt des chemins de sources à la génération ;
  l'amorce lance CMake depuis `<D>\work`.
- Non vérifié : un vrai PC Windows (antivirus/SmartScreen sur un exe non signé, durée réelle de compilation).

## Au-delà de J1 (historique)

Le jeu poursuit l'initialisation D3D (`VdSetGraphicsInterruptCallback`,
`VdSetSystemCommandBufferGpuIdentifierAddress`, `VdInitializeRingBuffer`,
`VdEnableRingBufferRPtrWriteBack`), crée un DPC puis un thread (`ExCreateThread`, entrée
`0x823F74C8`) et attend indéfiniment : thread non lancé et anneau GPU jamais consommé.
C'est exactement le périmètre de J2 (threads, attentes) et J3 (anneau de commandes).

## Limites connues

- Un seul verrou noyau global (simple, potentiellement lent) ; suspension d'un autre thread appliquée à son prochain appel noyau seulement.
- Aucune libération mémoire (tas à pointeur croissant) ; `NtAllocateVirtualMemory` avec adresse
  imposée accepte sans vérifier la réservation.
- `RtlImageXexHeaderField` : clés `xx01` interprétées comme « adresse de la valeur » (à confirmer).
- `XGetLanguage` renvoie 2 (à confirmer contre la table des langues XDK) ; `ExGetXConfigSetting`
  ne connaît que la catégorie 3 (journalise les autres).
- Champs PCR/KTHREAD : seuls ceux lus par le jeu sont remplis.
- Partie : sauvegarde vérifiée avec un point de contrôle ajouté par une TU de test, pas encore avec un point de
  contrôle d'origine (fosse de l'oracle) atteint en jouant.
- Linux seulement (`mmap`, `execinfo`) ; le portage Windows arrive en J4.
