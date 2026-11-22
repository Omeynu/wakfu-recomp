# État du projet

Ce document décrit ce qui fonctionne, comment c'est construit et ce qui manque. Méthode de la recompilation
et vérifications : `methode.md`. Analyse du binaire (en-têtes, imports, arborescence) : `analyse.md`.
Liste des imports noyau/XAM et de leur état : `runtime/imports.md`.

## En bref

- Le code PowerPC du jeu (XEX + mise à jour 2) est traduit en C++ par XenonRecomp patché : 15 014 fonctions,
  compilées sans avertissement (Clang, `-O2`).
- Le runtime fait tourner ce code : démarrage, vidéos, menus, nouvelle partie, première zone jouable,
  sauvegardes, son, manette. Windows x64 et Linux.
- Cadence : ≈ 30 à 55 images/s en jeu sur Radeon RX 6700 XT (Linux). Sous 60 images/s, le jeu tourne au
  ralenti (pas fixe de 1/60 s par image).

## Jalons

Les commentaires du code y font référence (« jalon 1 », « J4 »…).

| Jalon | État | Test |
|---|---|---|
| J1 — Démarrage sans affichage | Réussi | `runtime/tests/j1.sh` : le jeu atteint `VdInitializeEngines` sans plantage |
| J2 — Noyau HLE, boucle principale | Réussi | `runtime/tests/j2.sh` : 1 200 `VdSwap`, fichiers `Media` ouverts, aucun stub appelé |
| J3 — Processeur de commandes GPU | Réussi | `runtime/tests/j3.sh` : draws, resolves et shaders suivis, capture PNG |
| J4 — Rendu Vulkan | Réussi | `runtime/tests/j4.sh` : écran-titre et menus rendus, comparés par captures |
| J5 — Audio, vidéo, entrées, sauvegardes | Réussi | `j5-partie.sh` (partie lancée, personnage déplacé), `j5-sauvegarde.sh` |
| J6 — Distribution | Réussi | installateur Windows en un fichier, paquet Linux |

Batterie de non-régression : `runtime/tests/batterie.sh` (un résultat par ligne).

## Recompilation (XenonRecomp)

Ce qu'il a fallu corriger ou ajouter pour ce jeu :

1. **Extraction STFS** : aucun outil Linux maintenu, d'où `scripts/stfs_extract.py` (chaque bloc vérifié par
   SHA-1), puis `tools-src/wakfu-prepare` (STFS, ISO/XISO XGD1-3, dossier).
2. **Tables de sauts** : XenonAnalyse n'en trouvait qu'une (les `nop` de ce compilateur cassent ses motifs).
   Détecteur par émulation (`scripts/find_switches.py`) : 140 automatiques + 7 relues à la main.
3. **17 fonctions sans `.pdata`** (fonctions feuilles contenant une table, « switch » en chaîne de `bdzf`) :
   déclarées dans `config/wakfu.toml`.
4. **52 instructions non traduites** (7 868 occurrences) : ajoutées par `patches/xenonrecomp-wakfu.patch`,
   validées par un test différentiel contre QEMU (`tests/insn`, 66 cas, 0 échec). Aussi : `bdnzf` généralisé,
   `mftb` relié à une horloge à 50 MHz (au lieu de `__rdtsc()` brut, qui faussait le temps du jeu).
5. **Funclets `__finally`** de MSVC : ils lisent les registres de leur fonction parente ; ces 167 fonctions
   gardent leurs registres dans le contexte (`registers_in_context`, `scripts/find_context_functions.py`).
6. **Piège** : XenonRecomp n'efface pas les anciens `ppc_recomp.N.cpp` ; toujours vider `out/ppc` avant.

Options d'optimisation de XenonRecomp : désactivées (elles abîmaient la vidéo de l'écran-titre, `methode.md` §10).

## Runtime

- **Mémoire** : 4 Go d'espace invité, image chargée à `0x82000000`, mémoire physique vue aussi en `0xC0000000`.
  Tas à pointeur croissant (pas de libération).
- **Noyau / XAM** (`runtime/src/kernel/`) : threads hôtes, objets attendables en mémoire invitée, sections
  critiques, TLS, temps, fichiers (`game:\` → jeu de base, `update:\` → mise à jour, `save:\` → dossier de
  données), profil local, contenus et sauvegardes, réseau hors ligne. Imports non implémentés : stubs journalisés.
- **GPU** : processeur de commandes PM4 (`kernel/gpu_cp.cpp`), vblank à 60 Hz, interruptions. Rendu Vulkan
  (`gpu/renderer.cpp`) : shaders Xenos traduits en GLSL puis SPIR-V (analyse du microcode reprise de Xenia),
  textures décodées sur GPU (tuilage, formats), cibles EDRAM, resolves, profondeur et stencil, MSAA ramené à
  1X, soumission asynchrone. Les pages d'où une texture est décodée sont surveillées en écriture pour ne la
  redécoder que si elle change.
- **Présentation** : fenêtre SDL2, mise à l'échelle avec bandes noires, AMD FSR 1 en option.
- **Audio** : pilote de rendu XAudio émulé, décodage XMA par FFmpeg (décodeur xma2 seul).
- **Entrées** : manette SDL (branchement à chaud, vibrations), clavier de secours.
- **Réglages** : `config.toml` (`options.md`).
- **Diagnostics** : `WAKFU_TRACE=1` (imports appelés), `WAKFU_GAMELOG=1` (journal interne du jeu),
  `WAKFU_CAPTURE` (captures PNG), `WAKFU_GPU_PROFIL=1` (temps GPU par catégorie), gestionnaire de plantage.

### Erreurs instructives

- `NtAllocateVirtualMemory` Xbox n'a pas de paramètre `ZeroBits` ; et `MEM_HEAP` (0x40000000) lu à tort
  comme « pages de 16 Mo » épuisait l'espace virtuel au lancement d'une partie.
- `KeTimeStampBundle` doit être mis à jour (toutes les ms) : sinon `GetTickCount` reste à 0 et le lecteur
  vidéo XUI se fige.
- Le rappel d'interruption efface le bit du CPU qui l'exécute : l'appeler pour chaque CPU du masque.
- `NtSuspendThread` sur soi-même doit vraiment bloquer (réservoirs de threads du jeu).
- Cache de textures : le jeu réécrit plusieurs fois par image la même zone mémoire ; une texture ne peut être
  réutilisée que si sa mémoire n'a pas changé.

## Performances

La cadence est limitée par la chaîne processeur de commandes → GPU, exécutée en série : environ 9 fois par
image, le processeur de commandes attend une valeur que le GPU écrit à la fin de son travail. Le jeu suit la
synchro verticale comme la console : au-delà de 16,7 ms de travail, l'image prend 33,3 ms.

Gains principaux : textures décodées sur GPU et réutilisées d'une image à l'autre, indices lus directement par
le vertex shader (≈ 850 000 par image), interruptions du processeur de commandes immédiates. Le script de
mesure est `runtime/tests/perf/bench-joueur.py`.

## Limites connues

- Moins de 60 images/s sur beaucoup de machines, donc jeu ralenti.
- Testé surtout sous Linux (RADV, lavapipe) et sous Wine ; premiers essais sur PC Windows.
- L'exe Windows n'est pas signé (avertissement SmartScreen).
- Sauvegarde vérifiée avec un point de contrôle ajouté par une mise à jour de test, pas encore en jouant
  jusqu'au premier point de contrôle d'origine.
- Un seul verrou noyau global ; aucune libération de mémoire invitée.
- 15 shaders du catalogue ne se précompilent pas (format de sommet non géré ; non utilisés en zone 1).
- `XGetLanguage` et `ExGetXConfigSetting` partiellement émulés.
- Réseau absent (le service de matchmaking du jeu n'existe plus).
