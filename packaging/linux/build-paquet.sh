#!/bin/bash
# Construit WakfuRecomp-linux.tar.zst : script d'installation + sources (runtime, wakfu-prepare, XenonRecomp patché).
# AUCUN fichier du jeu ni C++ recompilé : tout ce qui vient du jeu est produit chez l'utilisateur (installer.sh).
# Usage : packaging/linux/build-paquet.sh <dossier de sortie>
#   Hôte : git, tar, zstd, réseau (clone de XenonRecomp au commit de tools.lock).
set -euo pipefail
REPO=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$(realpath -m "${1:?usage : build-paquet.sh <dossier de sortie>}")
ST=$OUT/stage/wakfu-recomp
rm -rf "$OUT/stage"; mkdir -p "$ST/src"
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
# XenonRecomp sans historique Git ni sorties de construction
tar -C "$XR" --exclude=.git --exclude='.wakfu-*' --exclude=build -cf - . | (mkdir -p "$ST/xenonrecomp" && tar -C "$ST/xenonrecomp" -xf -)
# Sources du dépôt : fichiers suivis par Git seulement
(cd "$REPO" && git ls-files -z runtime check config tools-src/wakfu-prepare tools-src/xexdump | xargs -0 cp --parents -t "$ST/src")
cp "$REPO/packaging/linux/installer.sh" "$ST/"
cp "$REPO/packaging/linux/LISEZMOI.txt" "$ST/"
chmod +x "$ST/installer.sh"
tar -C "$OUT/stage" -cf - wakfu-recomp | zstd -19 -T0 -q -f -o "$OUT/WakfuRecomp-linux.tar.zst"
echo "$OUT/WakfuRecomp-linux.tar.zst : $(du -h "$OUT/WakfuRecomp-linux.tar.zst" | cut -f1), commit $(git -C "$REPO" rev-parse --short HEAD)"
sha256sum "$OUT/WakfuRecomp-linux.tar.zst"
