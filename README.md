# wakfu-recomp

Préparation de la recompilation statique d'*Islands of Wakfu* (XBLA 584109AA, Xbox 360) vers PC,
à partir de la copie personnelle du jeu, avec XenonRecomp / XenosRecomp. Le runtime (rendu,
audio, entrées, noyau HLE) est hors périmètre : voir `docs/rapport.md`.

**Dépôt privé** : il ne contient jamais les fichiers du jeu ni le C++ généré (œuvre dérivée).
Le C++ généré est publié en archive à part (non inclus ici).

## Contenu

| Chemin | Rôle |
|---|---|
| `PROMPT.md` | Prompt d'origine (étapes 0 à 9) |
| `docs/rapport.md` | État de chaque étape, imports à émuler, travail restant |
| `docs/analyse.md` | Analyse du XEX patché (en-têtes, XDK, imports, arborescence) |
| `docs/methode.md` | Méthode et vérifications de chaque élément de la config |
| `config/wakfu.toml` | Config XenonRecomp |
| `config/jump_tables.toml` | Tables de sauts (générées) ; `switches_manual.toml` : cas relus à la main |
| `patches/xenonrecomp-wakfu.patch` | 52 instructions ajoutées à XenonRecomp |
| `scripts/` | Extraction STFS, analyse XEX, fonctions d'aide, tables de sauts |
| `tools-src/xexdump/` | Applique la TU, vide l'image du XEX (lié à XenonUtils) |
| `tools-src/wakfu-prepare/` | Prépare le jeu : STFS / ISO-XISO / dossier → base, tu2, patched (tests : `ctest`) |
| `tools-src/wakfu-installer/`, `packaging/windows/` | Installateur Windows en un fichier `WakfuRecomp.exe` (docs/build-windows.md §0) |
| `tests/insn/` | Test différentiel des instructions ajoutées (QEMU vs C++ recompilé) |
| `check/` | Vérification de compilation et d'édition de liens du C++ généré |
| `tools.lock` | Commits épinglés des outils |

## Reproduire (Linux, Clang ≥ 18)

```sh
# Espace de travail hors du dépôt ; `work` est un lien ignoré par Git
mkdir -p ~/wakfu-recomp && ln -s ~/wakfu-recomp work
apt-get install clang lld cmake ninja-build git python3 binutils-powerpc64-linux-gnu qemu-user zstd

# Outils aux commits de tools.lock, patch appliqué
git clone --recursive https://github.com/hedge-dev/XenonRecomp work/tools/XenonRecomp
git -C work/tools/XenonRecomp checkout <commit de tools.lock>
git -C work/tools/XenonRecomp apply "$PWD/patches/xenonrecomp-wakfu.patch"
CC=clang CXX=clang++ cmake -S work/tools/XenonRecomp -B work/tools/XenonRecomp/build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C work/tools/XenonRecomp/build

# Jeu (copie personnelle) : paquets STFS (jeu + TU 2) dans work/game/raw/, ou image ISO/XISO,
# ou dossier extrait. wakfu-prepare (tools-src/wakfu-prepare) détecte le format, extrait, applique
# la TU et écrit work/game/{base,tu2,patched/{default.xex,image.bin,patched.txt}}.
CC=clang CXX=clang++ cmake -S tools-src/wakfu-prepare -B work/build-prepare -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DXENONRECOMP_DIR=$PWD/work/tools/XenonRecomp && ninja -C work/build-prepare
work/build-prepare/wakfu-prepare -o work/game work/game/raw
# (équivalent manuel : scripts/stfs_extract.py pour base/ et tu2/, puis tools-src/xexdump
#  <base/default.xex> <patched/image.bin> <tu2/default.xexp> <patched/default.xex> > patched/patched.txt)

# Tables de sauts
python3 -I scripts/find_switches.py work/game/patched/image.bin work/game/patched/patched.txt \
    config/switches_manual.toml config/jump_tables.toml

# Génération : VIDER work/out/ppc avant (XenonRecomp ne supprime pas les anciens fichiers)
work/tools/XenonRecomp/build/XenonRecomp/XenonRecomp config/wakfu.toml work/tools/XenonRecomp/XenonUtils/ppc_context.h

# Vérifications (S = work/tools/XenonRecomp)
python3 -I tests/insn/run_tests.py $S/build/XenonRecomp/XenonRecomp $S/XenonUtils $S/thirdparty/simde work/tests-insn
CXX=clang++ cmake -S check -B work/build-check -G Ninja -DPPC_DIR=$PWD/work/out/ppc -DSIMDE_DIR=$PWD/$S/thirdparty/simde
ninja -C work/build-check
SIMDE_DIR=$PWD/$S/thirdparty/simde python3 -I check/link_check.py work/build-check/libwakfu_ppc.a \
    work/game/patched/patched.txt work/out/ppc work/link-check
```
