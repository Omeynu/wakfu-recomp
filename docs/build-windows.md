# Build Windows x64

Chaîne validée : **Clang + MinGW-w64 + lld** (la même que la compilation croisée testée sous Wine
depuis Linux). Sur le PC Windows, l'environnement le plus simple qui la fournit est MSYS2 « CLANG64 ».

## 0. Installateur en un fichier (recommandé) : `WakfuRecomp.exe`

Exécutable Windows x64 unique, à construire sous Linux (`packaging/windows/build-installer.sh <dossier>`,
téléchargements épinglés et vérifiés par SHA-256 dans `packaging/windows/deps.lock`). Il ne contient aucun
fichier du jeu ni C++ recompilé. Sur le PC du joueur :

```bat
WakfuRecomp.exe                                                           (fenêtre du lanceur : double-clic)
WakfuRecomp.exe <paquet ou ISO ou dossier du jeu> [<paquet de la TU 2>]   (ligne de commande, ou glisser-déposer sur l'exe)
```

Sans argument, une fenêtre (Dear ImGui sur SDL2, rendu 2D de SDL ; textes en français ou en anglais selon la langue
de Windows, tous dans `tools-src/wakfu-installer/textes.cpp`) guide le joueur : accueil,
champs Jeu et Mise à jour (paquet STFS, ISO/XISO ou dossier ; glisser-déposer ou « Parcourir », contrôle immédiat :
paquet Xbox 360, titre 584109AA, jeu ou mise à jour, TU 2, taille complète),
vérification (outils, `wakfu-prepare`, SHA-1 de l'image : refus d'une copie non reconnue), recompilation (étape,
progression de XenonRecomp et de ninja, temps restant estimé), réglages (toutes les clés de `config.toml`, écrites par
`runtime/src/config.cpp`), puis Jouer ; ensuite directement Jouer / Réglages / Recompiler. Journal :
`<D>\journal-lanceur.txt`. Avec des arguments, la ligne de commande est inchangée (sorties sur la console qui l'a
lancée, ou sur les sorties redirigées ; l'exécutable étant une application à fenêtre, `cmd.exe` n'attend pas sa fin :
`start /wait WakfuRecomp.exe ...`). Lancé par RecompLauncher (variable `RECOMPLAUNCHER=1`) : ni console ni attente
d'Entrée, outils et jeu sans fenêtre, lignes sur les sorties standard héritées avec la progression
`progress <0..1> <étape>` (outils 0–0,05, préparation 0,05–0,15, XenonRecomp 0,15–0,3, configuration et compilation
0,3–1 au rythme de ninja), copiées dans `<données>\journal-installation.log` (tronqué à chaque lancement) ; journal du
jeu toujours écrit (`journal.log`) ; code de sortie 1 si l'installation échoue, sinon celui du jeu. Étape `prepare` de RecompLauncher : `--sans-lancer <jeu> <mise à jour>` ; `%LOCALAPPDATA%\WakfuRecomp\pret` est retiré au début de la préparation ou de la compilation et écrit à la fin. Mode de test caché : `WAKFURECOMP_CAPTURE=<dossier>` enregistre une capture PNG de
chaque écran (états simulés ; `WAKFURECOMP_CAPTURE_REEL=1` : vérification et recompilation réelles), avec
`WAKFURECOMP_CAPTURE_JEU`, `_TU`, `_MAUVAIS` (fichier refusé), `_REFUS` (`<champ><chemin>|...`, champ 0 jeu ou
1 mise à jour : une capture par fichier refusé) et `_IMAGES` (dossier `base` du jeu préparé).
Codes de sortie de `wakfu-prepare` : 0 prêt, 1 erreur, 2 usage, 3 prêt mais SHA-1 de l'image non reconnu (version du
jeu ou de la mise à jour) : la ligne de commande de `WakfuRecomp.exe` continue (avertissement), la fenêtre refuse la
copie. Avec des arguments, les entrées sont d'abord contrôlées comme dans la fenêtre (refus immédiat, code 1).

Étapes (dossier `%LOCALAPPDATA%\WakfuRecomp`, ou `--dossier <D>`, ~3 Go libres) : extraction de l'archive
interne (zstd) dans `outils-<id>` (llvm-mingw 20250114 = LLVM 19.1.7 réduit à x86_64, CMake 3.31.6, ninja,
`XenonRecomp.exe` patché, `wakfu-prepare.exe`, sources du runtime, glslang/SDL2/FFmpeg précompilés,
en-têtes Vulkan), `wakfu-prepare`, XenonRecomp vers `work/out/ppc`, stubs d'imports (portage C++ de
`gen_imports.py`, sorties identiques vérifiées), CMake + ninja, puis `wakfu.exe` et `jouer.cmd` dans `<D>`.
Les étapes faites sont reprises (relancer après une erreur ; pour tout recompiler, supprimer `<D>\wakfu.exe`
et `<D>\work\build`). Durée mesurée sous Wine (8 cœurs) : < 3 min au total. Licences : `LICENCES-TIERS.txt` + `licences/`.
Test sous Wine : `wine WakfuRecomp.exe --sans-lancer Z:\…\game\raw`, puis `WAKFU_WIN_EXE=<D>/wakfu.exe runtime/tests/j1.sh …`.

## 1. Outils (MSYS2, fenêtre « MSYS2 CLANG64 »)

```sh
pacman -S --needed git python mingw-w64-clang-x86_64-{clang,lld,cmake,ninja,SDL2,vulkan-devel}
```

## 2. Sources et fichiers du jeu

```sh
git clone https://github.com/Omeynu/wakfu-recomp && cd wakfu-recomp
mkdir -p ../wakfu-work && ln -s ../wakfu-work work      # ou un dossier « work » à la place du lien
```

- Jeu : votre copie, préparée par `wakfu-prepare` (voir [`BUILDING.md`](../BUILDING.md), étape 2) dans `work/game/`.
- C++ généré : XenonRecomp patché (même fichier, étape 3), dans `work/out/ppc/`.
- Stubs d'imports : `python runtime/tools/gen_imports.py work/game/patched/default.xex work/game/patched/patched.txt <XenonUtils/xbox> work/runtime-gen`.

## 3. Compilation

```sh
cmake -S runtime -B work/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DPPC_DIR=$PWD/work/out/ppc -DSIMDE_DIR=<XenonRecomp>/thirdparty/simde -DGEN_DIR=$PWD/work/runtime-gen
ninja -C work/build wakfu
work/build/wakfu.exe work/game/patched/image.bin work/game/patched/default.xex work/game/base work/game/tu2 work/userdata
```

## Notes

- La mémoire invitée (4 Go) est réservée **et engagée** d'un bloc (`VirtualAlloc`) : il faut au moins
  4 Go de mémoire virtuelle disponible (RAM + fichier d'échange).
- Sous Windows, les piles de plantage donnent des adresses (`exe+0x…`) et non des noms ; `WAKFU_DUMP_AFTER`
  est sans effet.
- Compilation croisée depuis Linux : `cmake -DCMAKE_TOOLCHAIN_FILE=cmake/windows-mingw-clang.cmake …`,
  tests sous Wine : `WAKFU_WIN_EXE=<wakfu.exe> runtime/tests/j2.sh runtime/tests/wine-wakfu.sh …`.
  Le rendu demande glslang compilé pour MinGW (sinon le runtime est construit sans rendu) :
  ```sh
  git clone --depth 1 -b 15.1.0 https://github.com/KhronosGroup/glslang.git <src>
  cmake -S <src> -B <b> -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/windows-mingw-clang.cmake \
        -DCMAKE_INSTALL_PREFIX=<glslang-mingw> -DENABLE_OPT=OFF -DENABLE_HLSL=OFF -DGLSLANG_TESTS=OFF \
        -DENABLE_GLSLANG_BINARIES=OFF -DBUILD_SHARED_LIBS=OFF
  ninja -C <b> install
  ```
  puis ajouter `-Dglslang_DIR=<glslang-mingw>/lib/cmake/glslang` à la configuration du runtime. Test du rendu
  sous Wine (winevulkan -> lavapipe) : `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json WAKFU_WIN_EXE=<wakfu.exe>
  runtime/tests/j4.sh runtime/tests/wine-wakfu.sh …` (réussi : 900 images, 88 draws, 18 resolves, ≈ 47 s).
- Son XMA (musiques) : FFmpeg facultatif, trouvé par pkg-config (`libavcodec`, `libavutil`, décodeur `xma2`) ; sans
  lui le runtime est construit sans décodage XMA. Non testé sous Windows : le plus simple est une libavcodec
  statique minimale (configuration LGPL par défaut, sans `--enable-gpl`) :
  `configure --target-os=mingw32 --arch=x86_64 --cc=clang --enable-cross-compile --disable-everything
  --enable-decoder=xma2 --disable-programs --disable-avformat --disable-swresample --disable-swscale
  --disable-avfilter --disable-avdevice --enable-static --disable-shared --prefix=<ffmpeg-mingw>` puis
  `PKG_CONFIG_PATH=<ffmpeg-mingw>/lib/pkgconfig` à la configuration. Licence LGPL 2.1 : distribuer la source
  de FFmpeg (version + options) et sa licence avec le binaire ; la liaison statique impose de permettre la
  reliaison (les sources du runtime sont publiques, ou fournir les objets), sinon utiliser `avcodec-61.dll`.
- Un build MSVC/clang-cl n'a pas été essayé (non nécessaire avec MinGW).
