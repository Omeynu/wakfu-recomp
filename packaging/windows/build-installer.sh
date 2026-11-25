#!/bin/bash
# Construit WakfuRecomp.exe : lanceur et installateur Windows x64 en un seul fichier (amorce + archive zstd ajoutée à la fin).
# L'archive contient la chaîne de compilation (llvm-mingw, CMake, ninja), XenonRecomp.exe (patché),
# wakfu-prepare.exe, XenosRecomp.exe + DXC officiel (shaders natifs), les sources du runtime et les bibliothèques précompilées (glslang, SDL3, FFmpeg minimal,
# en-têtes Vulkan). AUCUN fichier du jeu ni C++ recompilé : tout ce qui vient du jeu est produit chez l'utilisateur.
#
# Usage : packaging/windows/build-installer.sh <dossier_de_construction> [<dossier jeu préparé, pour test>]
#   Hôte : Linux x86_64 avec cmake >= 3.20, ninja, make, curl, python3, zstd, git.
#   Les étapes déjà faites sont sautées (fichiers <construction>/bld/.fait-*) ; supprimer le dossier pour tout refaire.
#   Avec un dossier de jeu préparé (base/, patched/) : vérifie que gen_imports (C++) = runtime/tools/gen_imports.py.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../.." && pwd)
HERE=$REPO/packaging/windows
B=$(realpath -m "${1:?usage : build-installer.sh <dossier_de_construction> [<jeu préparé>]}")
GAME=${2:-}
DL=$B/dl SRC=$B/src BLD=$B/bld DEPS=$B/deps STAGE=$B/stage LOG=$B/logs
JOBS=$(nproc)
# Chemins de la machine de construction retirés des binaires (__FILE__, débogage) : exe reproductible ailleurs
export CFLAGS="-ffile-prefix-map=$B=build -ffile-prefix-map=$REPO=src" CXXFLAGS="-ffile-prefix-map=$B=build -ffile-prefix-map=$REPO=src"
mkdir -p "$DL" "$SRC" "$BLD" "$DEPS" "$LOG"
XENON_COMMIT=$(awk '$1 == "XenonRecomp" {print $3}' "$REPO/tools.lock")
LLVM_TAG=20250114

fetch() {   # <fichier> : téléchargé une fois, somme SHA-256 de deps.lock vérifiée à chaque fois
    local name=$1 sha url
    read -r _ sha url < <(grep -E "^$name " "$HERE/deps.lock") || { echo "absent de deps.lock : $name" >&2; exit 1; }
    if [ ! -f "$DL/$name" ]; then
        curl -fsSL --retry 3 -o "$DL/$name.part" "$url"
        mv "$DL/$name.part" "$DL/$name"
    fi
    echo "$sha  $DL/$name" | sha256sum -c --quiet - || { echo "SOMME INVALIDE : $name (fichier supprimé)" >&2; rm -f "$DL/$name"; exit 1; }
}
unpack() {  # <archive> <dossier> : extraction propre
    rm -rf "$2"; mkdir -p "$2"
    case $1 in
        *.zip) python3 -I -m zipfile -e "$DL/$1" "$2" ;;
        *) tar -xf "$DL/$1" -C "$2" --strip-components=1 ;;
    esac
}
propre() {   # <source> <build> : un cache CMake fait depuis un autre emplacement (dossier copié) bloque cmake
    grep -qsxF "CMAKE_HOME_DIRECTORY:INTERNAL=$1" "$2/CMakeCache.txt" || rm -rf "$2"
}
todo() { if [ -f "$BLD/.fait-$1" ]; then echo "== $1 : déjà fait"; return 1; fi; echo "== $1"; }
fait() { touch "$BLD/.fait-$1"; }
run() {     # <journal> <commande...> : sortie dans le journal, fin du journal si échec
    local log=$LOG/$1; shift
    "$@" >> "$log" 2>&1 || { echo "ÉCHEC ($*) ; fin de $log :" >&2; tail -20 "$log" >&2; exit 1; }
}

while read -r name _; do
    case $name in ''|'#'*) continue ;; esac
    fetch "$name"
done < "$HERE/deps.lock"

HOST=$B/llvm-mingw-linux
export LLVM_MINGW=$HOST
TC=$HERE/llvm-mingw-cross.cmake
if todo llvm-linux; then unpack llvm-mingw-$LLVM_TAG-ucrt-ubuntu-20.04-x86_64.tar.xz "$HOST"; fait llvm-linux; fi
CROSS=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=$TC "-DCMAKE_EXE_LINKER_FLAGS=-static -Wl,--no-insert-timestamp")   # PE sans date : reproductible

if todo glslang; then
    unpack glslang-15.1.0.tar.gz "$SRC/glslang"
    run glslang.log cmake -S "$SRC/glslang" -B "$BLD/glslang" "${CROSS[@]}" -DCMAKE_INSTALL_PREFIX="$DEPS/glslang" \
        -DENABLE_OPT=OFF -DENABLE_HLSL=OFF -DGLSLANG_TESTS=OFF -DENABLE_GLSLANG_BINARIES=OFF -DBUILD_SHARED_LIBS=OFF
    run glslang.log ninja -C "$BLD/glslang" install
    fait glslang
fi
if todo sdl3-3.4.20; then
    unpack SDL3-3.4.20.tar.gz "$SRC/SDL3"
    run sdl3.log cmake -S "$SRC/SDL3" -B "$BLD/sdl3" "${CROSS[@]}" -DCMAKE_INSTALL_PREFIX="$DEPS/SDL3" \
        -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
    run sdl3.log ninja -C "$BLD/sdl3" install
    fait sdl3-3.4.20
fi
if todo zstd; then   # pour l'amorce seulement (décompression de l'archive)
    unpack zstd-1.5.7.tar.gz "$SRC/zstd"
    run zstd.log cmake -S "$SRC/zstd/build/cmake" -B "$BLD/zstd" "${CROSS[@]}" -DCMAKE_INSTALL_PREFIX="$DEPS/zstd" \
        -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF \
        -DZSTD_LEGACY_SUPPORT=OFF -DZSTD_MULTITHREAD_SUPPORT=OFF
    run zstd.log ninja -C "$BLD/zstd" install
    fait zstd
fi
FFMPEG_OPTS=(--target-os=mingw32 --arch=x86_64 --enable-cross-compile --cross-prefix=x86_64-w64-mingw32-
    --cc=x86_64-w64-mingw32-clang --ar=llvm-ar --ranlib=llvm-ranlib --nm=llvm-nm --strip=llvm-strip
    --disable-everything --enable-decoder=xma2 --disable-programs --disable-doc --disable-avformat
    --disable-swresample --disable-swscale --disable-avfilter --disable-avdevice --disable-x86asm
    --enable-static --disable-shared --disable-autodetect --pkg-config=false)
if todo ffmpeg; then   # LGPL 2.1 (pas de --enable-gpl / --enable-nonfree)
    unpack ffmpeg-7.1.2.tar.xz "$SRC/ffmpeg"
    rm -rf "$BLD/ffmpeg"; mkdir -p "$BLD/ffmpeg"
    (cd "$BLD/ffmpeg" && PATH=$HOST/bin:$PATH run ffmpeg.log "$SRC/ffmpeg/configure" --prefix="$DEPS/ffmpeg" "${FFMPEG_OPTS[@]}" \
        && PATH=$HOST/bin:$PATH run ffmpeg.log make -j"$JOBS" install)
    fait ffmpeg
fi
if todo vulkan; then
    unpack Vulkan-Headers-1.4.309.tar.gz "$SRC/vulkan"
    rm -rf "$DEPS/vulkan"; mkdir -p "$DEPS/vulkan"; cp -r "$SRC/vulkan/include" "$DEPS/vulkan/"
    fait vulkan
fi
XR=$SRC/XenonRecomp
# Étape refaite quand le patch ou le commit change (empreinte dans le nom du marqueur)
XR_KEY=xenonrecomp-src-$( (echo "$XENON_COMMIT"; cat "$REPO/patches/xenonrecomp-wakfu.patch") | sha256sum | cut -c1-16)
if todo "$XR_KEY"; then   # commit de tools.lock + patch du dépôt
    rm -rf "$XR"
    run xenon.log git clone -q https://github.com/hedge-dev/XenonRecomp "$XR"
    run xenon.log git -C "$XR" checkout -q "$XENON_COMMIT"
    run xenon.log git -C "$XR" submodule update -q --init --recursive
    run xenon.log git -C "$XR" apply "$REPO/patches/xenonrecomp-wakfu.patch"
    fait "$XR_KEY"
fi
# XenonRecomp.exe et wakfu-prepare.exe : même arbre (wakfu-prepare inclut XenonRecomp, casse de Windows.h réglée)
propre "$REPO/tools-src/wakfu-prepare" "$BLD/tools"
run tools.log cmake -S "$REPO/tools-src/wakfu-prepare" -B "$BLD/tools" "${CROSS[@]}" -DXENONRECOMP_DIR="$XR"
run tools.log ninja -C "$BLD/tools" wakfu-prepare XenonRecomp
# XenosRecomp.exe (sources au commit de tools.lock, sans le sous-module tiers dxc-bin) lié au DXC officiel, et xr_hash.exe
XS=$SRC/XenosRecomp
XENOS_COMMIT=$(awk '$1 == "XenosRecomp" {print $3}' "$REPO/tools.lock")
if todo "xenosrecomp-$XENOS_COMMIT"; then
    rm -rf "$XS"
    run xenos.log git clone -q https://github.com/hedge-dev/XenosRecomp "$XS"
    run xenos.log git -C "$XS" checkout -q "$XENOS_COMMIT"
    run xenos.log git -C "$XS" submodule update -q --init thirdparty/fmt thirdparty/smol-v thirdparty/xxHash thirdparty/zstd
    fait "xenosrecomp-$XENOS_COMMIT"
fi
DXW=$SRC/dxc
if todo dxc-2026_09_29; then   # chemins de l'archive Microsoft séparés par « \ »
    rm -rf "$DXW"
    python3 -I -c 'import os, sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
for i in z.infolist():
    n = os.path.normpath(i.filename.replace("\\", "/"))
    if i.is_dir() or n.startswith(("..", "/")): continue
    p = os.path.join(sys.argv[2], n); os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, "wb").write(z.read(i))' "$DL/dxc_2026_09_29.zip" "$DXW"
    fait dxc-2026_09_29
fi
propre "$REPO/packaging/shaders/xenosrecomp" "$BLD/xenos"
run xenos.log cmake -S "$REPO/packaging/shaders/xenosrecomp" -B "$BLD/xenos" "${CROSS[@]}" -DXENOSRECOMP_DIR="$XS" -DDXC_DIR="$DXW"
run xenos.log ninja -C "$BLD/xenos" XenosRecomp xr_hash
# Fenêtre du lanceur (amorce) : Dear ImGui (sources) et police Inter (Regular, SemiBold), avec SDL3 et zstd ci-dessus
if todo imgui-1.93.0; then unpack imgui-1.93.0.tar.gz "$SRC/imgui"; fait imgui-1.93.0; fi
if todo inter-4.1; then unpack Inter-4.1.zip "$SRC/inter"; fait inter-4.1; fi
# Amorce de l'installateur
propre "$REPO/tools-src/wakfu-installer" "$BLD/stub"
run stub.log cmake -S "$REPO/tools-src/wakfu-installer" -B "$BLD/stub" "${CROSS[@]}" -Dzstd_DIR="$DEPS/zstd/lib/cmake/zstd" \
    -DSDL3_DIR="$DEPS/SDL3/lib/cmake/SDL3" -DVULKAN_INCLUDE_DIR="$DEPS/vulkan/include" -DIMGUI_DIR="$SRC/imgui" -DINTER_DIR="$SRC/inter/extras/ttf" -DXENONRECOMP_DIR="$XR"
run stub.log ninja -C "$BLD/stub"
if [ -n "$GAME" ]; then   # gen_imports C++ (amorce) identique au script Python de référence
    propre "$REPO/tools-src/wakfu-installer" "$BLD/stub-linux"
    run stub.log cmake -S "$REPO/tools-src/wakfu-installer" -B "$BLD/stub-linux" -G Ninja -DCMAKE_BUILD_TYPE=Release
    run stub.log ninja -C "$BLD/stub-linux" test_gen_imports
    rm -rf "$BLD/gi-py" "$BLD/gi-cpp"
    python3 -I "$REPO/runtime/tools/gen_imports.py" "$GAME/patched/default.xex" "$GAME/patched/patched.txt" "$XR/XenonUtils/xbox" "$BLD/gi-py" > /dev/null
    "$BLD/stub-linux/test_gen_imports" "$GAME/patched/default.xex" "$GAME/patched/patched.txt" "$XR/XenonUtils/xbox" "$BLD/gi-cpp" > /dev/null
    diff -r "$BLD/gi-py" "$BLD/gi-cpp" > /dev/null && echo "gen_imports C++ = Python : OK" || { echo "ÉCHEC : gen_imports C++ diffère du Python" >&2; exit 1; }
fi

# ---- Arborescence de l'archive ----
echo "== assemblage"
rm -rf "$STAGE"; mkdir -p "$STAGE"/{bin,sdk/XenonUtils,sdk/thirdparty,src,licences}
# llvm-mingw Windows, réduit à la cible x86_64 et aux outils nécessaires (clang, lld, ar/ranlib, windres)
unpack llvm-mingw-$LLVM_TAG-ucrt-x86_64.zip "$BLD/llvm-win"
L=$STAGE/llvm-mingw
mv "$BLD/llvm-win"/llvm-mingw-* "$L"
rm -rf "$L"/{python,share,i686-w64-mingw32,armv7-w64-mingw32,aarch64-w64-mingw32,arm64ec-w64-mingw32} "$L"/lib/{libear,libscanbuild}
find "$L/lib/clang" -path '*/lib/windows/*' ! -name 'libclang_rt.builtins-x86_64.a' -delete
find "$L/bin" -type f ! \( -name 'clang.exe' -o -name 'clang++.exe' -o -name 'clang-[0-9]*.exe' -o -name 'ld.lld.exe' \
    -o -name 'lld.exe' -o -name 'llvm-ar.exe' -o -name 'llvm-ranlib.exe' -o -name 'llvm-rc.exe' -o -name 'llvm-windres.exe' \
    -o -name 'windres.exe' -o -name 'x86_64-w64-mingw32-windres.exe' -o -name 'x86_64-*.cfg' -o -name 'mingw32-common.cfg' \
    -o -name 'libLLVM-*.dll' -o -name 'libclang-cpp.dll' -o -name 'libc++.dll' -o -name 'libunwind.dll' -o -name 'libwinpthread-1.dll' \) -delete
cp "$L/LICENSE.TXT" "$STAGE/licences/llvm-mingw.txt"
# CMake (sans documentation ni interface graphique) et ninja
unpack cmake-3.31.6-windows-x86_64.zip "$BLD/cmake-win"
mv "$BLD/cmake-win"/cmake-* "$STAGE/cmake"
mkdir -p "$STAGE/licences/cmake"; (cd "$STAGE/cmake/doc/cmake" && find . -name "Copyright.txt" -exec cp --parents -t "$STAGE/licences/cmake" {} +)
rm -rf "$STAGE/cmake"/{doc,man} "$STAGE/cmake/bin"/{cmake-gui.exe,cmcldeps.exe,ctest.exe,cpack.exe}
python3 -I -m zipfile -e "$DL/ninja-win.zip" "$STAGE/bin"
cp "$DL/ninja-1.12.1-COPYING" "$STAGE/licences/ninja.txt"
cp "$BLD/tools/wakfu-prepare.exe" "$BLD/tools/xenonrecomp/XenonRecomp/XenonRecomp.exe" "$STAGE/bin/"
# En-têtes et tables de XenonRecomp nécessaires à la génération et à la compilation
cp "$XR/XenonUtils/ppc_context.h" "$STAGE/sdk/XenonUtils/"
cp -r "$XR/XenonUtils/xbox" "$STAGE/sdk/XenonUtils/"
cp -r "$XR/thirdparty/simde" "$STAGE/sdk/thirdparty/"
rm -rf "$STAGE/sdk/thirdparty/simde"/{.git,test}
mkdir -p "$STAGE/sdk/thirdparty/tomlplusplus"   # config.toml du runtime (en-têtes seulement)
cp -r "$XR/thirdparty/tomlplusplus/include" "$STAGE/sdk/thirdparty/tomlplusplus/"
mkdir -p "$STAGE/sdk/thirdparty/xxHash"   # empreintes des shaders (XXH3, en-tête seul)
cp "$XR/thirdparty/xxHash/xxhash.h" "$STAGE/sdk/thirdparty/xxHash/"
# Shaders natifs (--natif) : XenosRecomp, xr_hash, DXC officiel (dxc.exe, dxcompiler.dll)
mkdir -p "$STAGE/natif"
cp "$BLD/xenos/XenosRecomp/XenosRecomp.exe" "$BLD/xenos/xr_hash.exe" "$XS/XenosRecomp/shader_common.h" "$DXW"/bin/x64/{dxc.exe,dxcompiler.dll} "$STAGE/natif/"   # pas dxil.dll (signature DXIL, licence Microsoft) : SPIR-V seulement
# Bibliothèques précompilées
mkdir -p "$STAGE/deps"
cp -r "$DEPS"/{glslang,SDL3,ffmpeg,vulkan} "$STAGE/deps/"
rm -rf "$STAGE/deps/SDL3/share" "$STAGE/deps/ffmpeg/share"
# Sources du runtime (fichiers suivis par Git seulement : jamais de C++ généré ni de fichier du jeu)
(cd "$REPO" && git ls-files -z runtime check config | xargs -0 cp --parents -t "$STAGE/src")
# Licences tierces
cp "$XR/LICENSE.md" "$STAGE/licences/XenonRecomp.txt"
cp "$XR/thirdparty/fmt/LICENSE" "$STAGE/licences/fmt.txt"   # lié à XenonRecomp.exe
cp "$XR/thirdparty/simde/COPYING" "$STAGE/licences/simde.txt"
cp "$XR/thirdparty/tomlplusplus/LICENSE" "$STAGE/licences/tomlplusplus.txt"
cp "$XR/thirdparty/xxHash/LICENSE" "$STAGE/licences/xxHash.txt"
cp "$XR/thirdparty/libmspack/libmspack/COPYING.LIB" "$STAGE/licences/libmspack-LGPL-2.1.txt"
cp "$XR/thirdparty/tiny-AES-c/unlicense.txt" "$STAGE/licences/tiny-AES-c.txt"
sed -n '1,/\*\//p' "$XR/thirdparty/TinySHA1/TinySHA1.hpp" > "$STAGE/licences/TinySHA1.txt"
cp "$SRC/SDL3/LICENSE.txt" "$STAGE/licences/SDL3.txt"
cp "$SRC/zstd/LICENSE" "$STAGE/licences/zstd.txt"
cp "$SRC/imgui/LICENSE.txt" "$STAGE/licences/imgui.txt"
cp "$SRC/inter/LICENSE.txt" "$STAGE/licences/Inter-OFL-1.1.txt"
cp "$SRC/ffmpeg/COPYING.LGPLv2.1" "$STAGE/licences/ffmpeg-LGPL-2.1.txt"
cp "$SRC/glslang/LICENSE.txt" "$STAGE/licences/glslang.txt"
cp "$SRC/vulkan/LICENSE.md" "$STAGE/licences/Vulkan-Headers.txt"
cp "$REPO/runtime/third_party/xenia/LICENSE" "$STAGE/licences/xenia.txt"
cp "$XS/LICENSE.md" "$STAGE/licences/XenosRecomp.txt"
cp "$XS/thirdparty/smol-v/license.txt" "$STAGE/licences/smol-v.txt"
cp "$DXW/LICENSE-LLVM.txt" "$STAGE/licences/DirectXShaderCompiler-LLVM.txt"
cp "$DXW/LICENCE-MIT.txt" "$STAGE/licences/DirectXShaderCompiler-MIT.txt"
cp "$REPO/runtime/third_party/fidelityfx-fsr1/LICENSE.txt" "$STAGE/licences/FidelityFX-FSR.txt"
{
    cat "$HERE/LICENCES-TIERS.txt"
    echo; echo "Options de configuration de FFmpeg 7.1.2 (source : https://ffmpeg.org/releases/ffmpeg-7.1.2.tar.xz) :"
    echo "  ${FFMPEG_OPTS[*]}"
} > "$STAGE/LICENCES-TIERS.txt"

# ---- Archive et exécutable final ----
echo "== archive (zstd -19)"
python3 -I "$HERE/pack.py" "$STAGE" "$B/payload.id" | zstd -q -19 --long=27 -T0 -o "$B/payload.zst" -f
python3 -I "$HERE/pack.py" --append "$BLD/stub/wakfu-installer.exe" "$B/payload.zst" "$(cat "$B/payload.id")" "$B/WakfuRecomp.exe"
echo "WakfuRecomp.exe : $(du -m --apparent-size "$B/WakfuRecomp.exe" | cut -f1) Mo, archive $(cat "$B/payload.id"), arbre $(du -sm --apparent-size "$STAGE" | cut -f1) Mo"
sha256sum "$B/WakfuRecomp.exe"
