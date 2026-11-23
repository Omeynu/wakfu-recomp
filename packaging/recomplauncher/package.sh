#!/bin/sh
# Release zip for GitHub and RecompLauncher : WakfuRecomp.exe + recomplauncher.json at the top, plus licenses.
# Usage : packaging/recomplauncher/package.sh <WakfuRecomp.exe> <version, e.g. 0.1.0> <output dir>
set -eu
EXE=${1:?usage : package.sh <WakfuRecomp.exe> <version> <output dir>}; VER=${2:?version}; OUT=${3:?output dir}
case $VER in *[!0-9A-Za-z.-]*|'') echo "invalid version: $VER" >&2; exit 2 ;; esac
REPO=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$OUT"
ZIP=$OUT/wakfu-recomp-$VER-windows-x64.zip
rm -f "$ZIP"
python3 -I - "$ZIP" "$EXE" "$REPO" <<'PY'
import sys, zipfile, json
zip_path, exe, repo = sys.argv[1:]
json.load(open(f"{repo}/packaging/recomplauncher/recomplauncher.json"))   # refuse a broken manifest
with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
    z.write(exe, "WakfuRecomp.exe")
    z.write(f"{repo}/packaging/recomplauncher/recomplauncher.json", "recomplauncher.json")
    z.write(f"{repo}/COPYING", "COPYING.txt")
    z.write(f"{repo}/packaging/windows/LICENCES-TIERS.txt", "LICENCES-TIERS.txt")
PY
echo "$ZIP"
