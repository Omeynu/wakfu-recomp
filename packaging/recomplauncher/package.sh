#!/bin/sh
# Release files for one platform: the program alone (for players) and an archive for RecompLauncher
# (program + recomplauncher.json at the top + licenses).
# Usage : packaging/recomplauncher/package.sh <windows|linux> <program> <version, e.g. 0.1.0> <output dir>
#   windows: <program> = WakfuRecomp.exe -> WakfuRecomp-<v>-windows-x64.exe + wakfu-recomp-<v>-windows-x64.zip
#   linux:   <program> = WakfuRecomp     -> WakfuRecomp-<v>-linux-x86_64  + wakfu-recomp-<v>-linux-x86_64.tar.gz
set -eu
OS=${1:?usage : package.sh <windows|linux> <program> <version> <output dir>}; PROG=${2:?program}; VER=${3:?version}; OUT=${4:?output dir}
case $VER in *[!0-9A-Za-z.-]*|'') echo "invalid version: $VER" >&2; exit 2 ;; esac
[ -f "$PROG" ] || { echo "program not found: $PROG" >&2; exit 2; }
REPO=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$OUT"
python3 -I - "$OS" "$PROG" "$VER" "$OUT" "$REPO" <<'PY'
import io, json, os, shutil, sys, tarfile, zipfile
os_, prog, ver, out, repo = sys.argv[1:]
manifest = f"{repo}/packaging/recomplauncher/recomplauncher.json"
json.load(open(manifest))   # refuse a broken manifest
if os_ == "windows":
    single = f"{out}/WakfuRecomp-{ver}-windows-x64.exe"
    shutil.copyfile(prog, single)
    with zipfile.ZipFile(f"{out}/wakfu-recomp-{ver}-windows-x64.zip", "w", zipfile.ZIP_DEFLATED) as z:
        z.write(prog, "WakfuRecomp.exe")
        z.write(manifest, "recomplauncher.json")
        z.write(f"{repo}/COPYING", "COPYING.txt")
        z.write(f"{repo}/packaging/windows/LICENCES-TIERS.txt", "LICENCES-TIERS.txt")
elif os_ == "linux":
    single = f"{out}/WakfuRecomp-{ver}-linux-x86_64"
    shutil.copyfile(prog, single)
    os.chmod(single, 0o755)
    def add(t, path, name, mode):
        info = t.gettarinfo(path, name)
        info.mode, info.uid, info.gid, info.uname, info.gname = mode, 0, 0, "", ""
        with open(path, "rb") as f:
            t.addfile(info, f)
    with tarfile.open(f"{out}/wakfu-recomp-{ver}-linux-x86_64.tar.gz", "w:gz") as t:
        add(t, prog, "WakfuRecomp", 0o755)
        add(t, manifest, "recomplauncher.json", 0o644)
        add(t, f"{repo}/COPYING", "COPYING", 0o644)
        add(t, f"{repo}/packaging/linux/LISEZMOI.txt", "LISEZMOI.txt", 0o644)
else:
    sys.exit(f"unknown platform: {os_} (windows or linux)")
PY
ls "$OUT"
