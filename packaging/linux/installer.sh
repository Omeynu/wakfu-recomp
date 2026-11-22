#!/bin/bash
# Installe Islands of Wakfu (recompilé) sous Linux à partir des fichiers du jeu de l'utilisateur (CachyOS / Arch).
# Rien du jeu n'est fourni : le jeu est extrait, recompilé (XenonRecomp) et compilé ici, comme le fait WakfuRecomp.exe.
#
# Usage : ./installer.sh [--dossier <D>] [--sans-pacman] <jeu> <mise à jour>
#   <jeu>          : paquet STFS, image ISO/XISO ou dossier du jeu ; <mise à jour> : paquet de la TU 2.
#   --dossier <D>  : dossier d'installation (défaut : ~/.local/share/wakfu-recomp)
#   --sans-pacman  : ne pas installer les dépendances (déjà présentes, ou autre distribution)
# Relancer sans <jeu> ni <mise à jour> recompile seulement (jeu déjà préparé).
set -euo pipefail
ICI=$(cd "$(dirname "$0")" && pwd)
D=$HOME/.local/share/wakfu-recomp
PACMAN=1
ENTREES=()
while [ $# -gt 0 ]; do
    case $1 in
        --dossier) D=$(realpath -m "$2"); shift 2 ;;
        --sans-pacman) PACMAN=0; shift ;;
        -h|--help) sed -n 2,9p "$0"; exit 0 ;;
        *) ENTREES+=("$(realpath "$1")"); shift ;;
    esac
done
[ ${#ENTREES[@]} -eq 0 ] || [ ${#ENTREES[@]} -eq 2 ] || { echo "Attendu : <jeu> <mise à jour> (ou rien pour recompiler)" >&2; exit 2; }
W=$D/work G=$D/work/game O=$D/outils XR=$ICI/xenonrecomp SRC=$ICI/src
for e in "${ENTREES[@]}"; do   # le dossier de travail est vidé : aucune entrée dedans
    case $e/ in "$D"/*) echo "L'entrée $e est dans $D : la déplacer" >&2; exit 2 ;; esac
done
JOURNAL=$D/installation.log
mkdir -p "$D"
etape() { echo "[$1/5] $2"; }
lancer() {   # <nom> <commande...> : sortie dans le journal, fin du journal si échec
    local nom=$1; shift
    echo "== $nom : $*" >> "$JOURNAL"
    if ! "$@" >> "$JOURNAL" 2>&1; then echo "ÉCHEC : $nom (journal : $JOURNAL)" >&2; tail -15 "$JOURNAL" >&2; exit 1; fi
}

etape 1 "Outils (dépendances, XenonRecomp, wakfu-prepare)"
if [ $PACMAN = 1 ]; then
    PAQUETS=(clang cmake ninja python pkgconf sdl2-compat vulkan-headers vulkan-icd-loader glslang ffmpeg)
    MANQUANTS=$(pacman -T "${PAQUETS[@]}" || true)
    if [ -n "$MANQUANTS" ]; then
        echo "  Paquets à installer : $MANQUANTS"
        sudo pacman -S --needed $MANQUANTS
    fi
fi
ls /usr/share/vulkan/icd.d/radeon_icd*.json > /dev/null 2>&1 || echo "  Attention : pilote Vulkan Radeon (vulkan-radeon) introuvable ; un autre pilote Vulkan conviendra s'il existe."
lancer "configuration des outils" cmake -S "$SRC/tools-src/wakfu-prepare" -B "$O" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DXENONRECOMP_DIR="$XR"
lancer "compilation des outils" ninja -C "$O" wakfu-prepare XenonRecomp

if [ ${#ENTREES[@]} -eq 2 ]; then
    etape 2 "Préparation du jeu (extraction, mise à jour)"
    rm -rf "$G" "$W/out/.recomp-ok"
    code=0
    "$O/wakfu-prepare" -o "$G" "${ENTREES[@]}" >> "$JOURNAL" 2>&1 || code=$?
    # 3 : jeu préparé mais image non reconnue (avertissement dans le journal) ; autre code : échec
    [ $code -eq 0 ] || [ $code -eq 3 ] || { echo "ÉCHEC : préparation du jeu (journal : $JOURNAL)" >&2; tail -15 "$JOURNAL" >&2; exit 1; }
    [ $code -eq 0 ] || echo "  Attention : image du jeu non reconnue (voir $JOURNAL) ; on continue."
else
    [ -f "$G/patched/default.xex" ] || { echo "Jeu non préparé : donner <jeu> <mise à jour>" >&2; exit 2; }
    etape 2 "Jeu déjà préparé : $G"
fi

etape 3 "Recompilation du code PowerPC (XenonRecomp)"
rm -rf "$D/config" "$W/out/ppc" "$W/gen"
cp -r "$SRC/config" "$D/config"
mkdir -p "$W/out/ppc"
( cd "$D/config" && lancer "XenonRecomp" "$O/xenonrecomp/XenonRecomp/XenonRecomp" wakfu.toml "$XR/XenonUtils/ppc_context.h" )
lancer "imports" python3 -I "$SRC/runtime/tools/gen_imports.py" "$G/patched/default.xex" "$G/patched/patched.txt" "$XR/XenonUtils/xbox" "$W/gen"

etape 4 "Compilation (plusieurs minutes selon le processeur)"
lancer "configuration CMake" cmake -S "$SRC/runtime" -B "$W/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DPPC_DIR="$W/out/ppc" -DSIMDE_DIR="$XR/thirdparty/simde" -DGEN_DIR="$W/gen"
lancer "compilation" cmake --build "$W/build" --target wakfu

etape 5 "Installation"
cp "$W/build/wakfu" "$D/wakfu"
mkdir -p "$D/userdata"
cat > "$D/jouer.sh" <<FIN
#!/bin/sh
# Lance Islands of Wakfu (données et sauvegardes : $D/userdata ; journaux des 3 derniers lancements : userdata/journal*.txt)
cd "$D" || exit 1
mv -f userdata/journal.1.txt userdata/journal.2.txt 2>/dev/null; mv -f userdata/journal.txt userdata/journal.1.txt 2>/dev/null
exec ./wakfu work/game/patched/image.bin work/game/patched/default.xex work/game/base work/game/tu2 userdata "\$@" > userdata/journal.txt 2>&1
FIN
chmod +x "$D/jouer.sh"
mkdir -p "$HOME/.local/share/applications"
cat > "$HOME/.local/share/applications/wakfu-recomp.desktop" <<FIN
[Desktop Entry]
Type=Application
Name=Islands of Wakfu (recompilé)
Exec=$D/jouer.sh
Path=$D
Terminal=false
Categories=Game;
FIN
echo "Prêt : $D/jouer.sh (aussi dans le menu des applications : « Islands of Wakfu (recompilé) »)"
