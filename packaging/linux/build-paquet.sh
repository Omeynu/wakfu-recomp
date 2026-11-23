#!/bin/bash
# Construit WakfuRecomp-linux.tar.zst : dossier WakfuRecomp avec le lanceur WakfuRecomp (fenêtre Dear ImGui/SDL2, même
# programme que WakfuRecomp.exe) et LISEZMOI.txt. Archive zstd ajoutée à la fin du lanceur (packaging/windows/pack.py) :
# sources (runtime, wakfu-prepare, XenonRecomp patché, XenosRecomp) + DXC officiel (binaire Microsoft, shaders natifs).
# AUCUN fichier du jeu ni C++ recompilé : tout ce qui vient du jeu est produit chez l'utilisateur (lanceur).
# Usage : packaging/linux/build-paquet.sh <dossier de sortie>
#   Hôte : git, tar, zstd, curl, python3, clang, cmake, ninja, SDL2 et zlib (en-têtes), réseau (clones de XenonRecomp /
#   XenosRecomp et DXC aux versions de tools.lock ; zstd, Dear ImGui, Inter aux versions de packaging/windows/deps.lock).
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$(realpath -m "${1:?usage : build-paquet.sh <dossier de sortie>}")
ST=$OUT/stage DL=$OUT/dl BLD=$OUT/bld
rm -rf "$ST"; mkdir -p "$ST/src" "$DL" "$BLD"
fetch() {   # <fichier> : téléchargé une fois, somme SHA-256 de deps.lock vérifiée à chaque fois
    local name=$1 sha url
    read -r _ sha url < <(grep -E "^$name " "$REPO/packaging/windows/deps.lock") || { echo "absent de deps.lock : $name" >&2; exit 1; }
    [ -f "$DL/$name" ] || { curl -fsSL --retry 3 -o "$DL/$name.part" "$url" && mv "$DL/$name.part" "$DL/$name"; }
    echo "$sha  $DL/$name" | sha256sum -c --quiet - || { echo "SOMME INVALIDE : $name (fichier supprimé)" >&2; rm -f "$DL/$name"; exit 1; }
}
propre() {   # <source> <build> : un cache CMake fait depuis un autre emplacement bloque cmake
    grep -qsxF "CMAKE_HOME_DIRECTORY:INTERNAL=$1" "$2/CMakeCache.txt" || rm -rf "$2"
}
XENON_COMMIT=$(awk '$1 == "XenonRecomp" {print $3}' "$REPO/tools.lock")
XR=$OUT/xenonrecomp-src
if [ ! -f "$XR/.wakfu-$XENON_COMMIT-$(sha256sum < "$REPO/patches/xenonrecomp-wakfu.patch" | cut -c1-16)" ]; then
    rm -rf "$XR"
    git clone -q https://github.com/hedge-dev/XenonRecomp "$XR"
    git -C "$XR" checkout -q "$XENON_COMMIT"
    git -C "$XR" submodule update -q --init --recursive
    git -C "$XR" apply "$REPO/patches/xenonrecomp-wakfu.patch"
    touch "$XR/.wakfu-$XENON_COMMIT-$(sha256sum < "$REPO/patches/xenonrecomp-wakfu.patch" | cut -c1-16)"
fi
# XenonRecomp sans historique Git ni sorties de construction ; liens remplacés par leur cible (archive sans liens)
tar -C "$XR" --dereference --exclude=.git --exclude='.wakfu-*' --exclude=build -cf - . | (mkdir -p "$ST/xenonrecomp" && tar -C "$ST/xenonrecomp" -xf -)
# XenosRecomp : sources au commit de tools.lock, sans son sous-module tiers dxc-bin (remplacé par le DXC officiel)
XENOS_COMMIT=$(awk '$1 == "XenosRecomp" {print $3}' "$REPO/tools.lock")
XS=$OUT/xenosrecomp-src
if [ ! -f "$XS/.wakfu-$XENOS_COMMIT" ]; then
    rm -rf "$XS"
    git clone -q https://github.com/hedge-dev/XenosRecomp "$XS"
    git -C "$XS" checkout -q "$XENOS_COMMIT"
    git -C "$XS" submodule update -q --init thirdparty/fmt thirdparty/smol-v thirdparty/xxHash thirdparty/zstd
    touch "$XS/.wakfu-$XENOS_COMMIT"
fi
tar -C "$XS" --dereference --exclude=.git --exclude='.wakfu-*' --exclude=thirdparty/dxc-bin -cf - . | (mkdir -p "$ST/xenosrecomp" && tar -C "$ST/xenosrecomp" -xf -)
read -r _ DXC_SHA DXC_URL < <(grep -E '^DXC-linux ' "$REPO/tools.lock")
DXC_TGZ=$OUT/$(basename "$DXC_URL")
[ -f "$DXC_TGZ" ] || { curl -fsSL --retry 3 -o "$DXC_TGZ.part" "$DXC_URL" && mv "$DXC_TGZ.part" "$DXC_TGZ"; }
echo "$DXC_SHA  $DXC_TGZ" | sha256sum -c --quiet - || { echo "SOMME INVALIDE : $DXC_TGZ (supprimé)" >&2; rm -f "$DXC_TGZ"; exit 1; }
mkdir -p "$ST/natif/dxc"   # sans libdxil.so ni dxv (validation DXIL, licence Microsoft) : SPIR-V seulement
tar -xzf "$DXC_TGZ" -C "$ST/natif/dxc" --exclude=lib/libdxil.so --exclude="bin/dxv*" --exclude=LICENSE-MS.txt
# Sources du dépôt : fichiers suivis par Git seulement
(cd "$REPO" && git ls-files -z runtime check config tools-src/wakfu-prepare tools-src/xexdump packaging/shaders | xargs -0 cp --parents -t "$ST/src")

# Lanceur : zstd statique, Dear ImGui et police Inter (sources), SDL2 et zlib du système
for f in zstd-1.5.7.tar.gz imgui-1.93.0.tar.gz Inter-4.1.zip; do fetch "$f"; done
if [ ! -f "$BLD/.fait-zstd-1.5.7" ]; then
    rm -rf "$BLD/zstd-src" "$BLD/zstd"; mkdir -p "$BLD/zstd-src"
    tar -xf "$DL/zstd-1.5.7.tar.gz" -C "$BLD/zstd-src" --strip-components=1
    cmake -S "$BLD/zstd-src/build/cmake" -B "$BLD/zstd-bld" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang \
        -DCMAKE_INSTALL_PREFIX="$BLD/zstd" -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON -DZSTD_BUILD_PROGRAMS=OFF \
        -DZSTD_BUILD_TESTS=OFF -DZSTD_LEGACY_SUPPORT=OFF -DZSTD_MULTITHREAD_SUPPORT=OFF > "$BLD/zstd.log"
    ninja -C "$BLD/zstd-bld" install >> "$BLD/zstd.log"
    touch "$BLD/.fait-zstd-1.5.7"
fi
if [ ! -f "$BLD/.fait-imgui-inter" ]; then
    rm -rf "$BLD/imgui" "$BLD/inter"; mkdir -p "$BLD/imgui" "$BLD/inter"
    tar -xf "$DL/imgui-1.93.0.tar.gz" -C "$BLD/imgui" --strip-components=1
    python3 -I -m zipfile -e "$DL/Inter-4.1.zip" "$BLD/inter"
    touch "$BLD/.fait-imgui-inter"
fi
propre "$REPO/tools-src/wakfu-installer" "$BLD/stub"
cmake -S "$REPO/tools-src/wakfu-installer" -B "$BLD/stub" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ \
    -Dzstd_DIR="$BLD/zstd/lib/cmake/zstd" -DIMGUI_DIR="$BLD/imgui" -DINTER_DIR="$BLD/inter/extras/ttf" -DXENONRECOMP_DIR="$XR" > "$BLD/stub.log"
ninja -C "$BLD/stub" >> "$BLD/stub.log"
"$BLD/stub/test_suppression" "$(mktemp -d -p "$BLD")"
for l in "$BLD/imgui/LICENSE.txt:imgui" "$BLD/inter/LICENSE.txt:Inter-OFL-1.1" "$BLD/zstd-src/LICENSE:zstd"; do
    mkdir -p "$ST/licences"; cp "${l%%:*}" "$ST/licences/${l##*:}.txt"
done

# Lanceur final : amorce + archive
echo "== archive (zstd -19)"
python3 -I "$REPO/packaging/windows/pack.py" "$ST" "$OUT/payload.id" | zstd -q -19 --long=27 -T0 -o "$OUT/payload.zst" -f
P=$OUT/paquet/WakfuRecomp
rm -rf "$OUT/paquet"; mkdir -p "$P"
python3 -I "$REPO/packaging/windows/pack.py" --append "$BLD/stub/WakfuRecomp" "$OUT/payload.zst" "$(cat "$OUT/payload.id")" "$P/WakfuRecomp"
chmod +x "$P/WakfuRecomp"
cp "$REPO/packaging/linux/LISEZMOI.txt" "$P/"
A=$OUT/WakfuRecomp-linux.tar.zst
tar -C "$OUT/paquet" -cf - WakfuRecomp | zstd -19 -T0 -q -f -o "$A"
echo "$A : $(du -h --apparent-size "$A" | cut -f1), archive $(cat "$OUT/payload.id"), commit $(git -C "$REPO" rev-parse --short HEAD)"
sha256sum "$A"
