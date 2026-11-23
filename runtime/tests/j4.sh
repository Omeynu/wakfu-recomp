#!/bin/sh
# Jalon 4 (partiel) : rendu natif Vulkan de l'écran-titre. 1400 images (natif à 60 img/s : écran-titre en boucle),
# draws et resolves exécutés sans draw ignoré, capture de l'image 1400 non uniforme. Shaders : <dossier du binaire>/spirv
# ou WAKFU_NATIF_SPIRV.
# Usage : j4.sh <wakfu> <image.bin> <xex patché> <jeu de base> <tu2> <données> [journal]
set -u
exe=$1 image=$2 xex=$3 base=$4 tu=$5 data=$6 log=${7:-j4.log}
cap="${log%.log}-captures"; mkdir -p "$cap"
WAKFU_RENDER=1 WAKFU_STOP_AT_SWAP=1400 WAKFU_CAPTURE=1400 WAKFU_CAPTURE_DIR="$cap" timeout 600 "$exe" "$image" "$xex" "$base" "$tu" "$data" > "$log" 2>&1
rc=$?
fail=0
[ $rc -eq 0 ] || { echo "ÉCHEC : code de sortie $rc"; fail=1; }
grep -q '^\[rendu\] Vulkan' "$log" || { echo "ÉCHEC : rendu non initialisé"; fail=1; }
grep -q '^\[arret\] 1400 VdSwap' "$log" || { echo "ÉCHEC : 1400 images non atteintes"; fail=1; }
# lignes [natif N] (toutes les 60 images) : draws, rendus, resolves
drawn=$(grep -o '^\[natif [0-9]*\] draws [0-9]*, rendus [0-9]*' "$log" | awk '{d+=$4; r+=$6} END {print r+0, d-r}')
skipped=${drawn#* }; drawn=${drawn% *}
res=$(grep -o 'resolves [0-9]* (non faits' "$log" | awk '{s+=$2} END {print s+0}')
echo "Draws rendus (images échantillonnées) : $drawn ; ignorés : $skipped ; resolves : $res"
[ "$drawn" -gt 0 ] && [ "$res" -gt 0 ] || { echo "ÉCHEC : rien d'exécuté"; fail=1; }
[ "$skipped" -eq 0 ] || { echo "ÉCHEC : draws ignorés"; fail=1; }
grep -q '^\[plantage\]' "$log" && { echo "ÉCHEC : plantage"; fail=1; }
colors=$(python3 -I -c "
import sys; sys.path.insert(0, '$(dirname "$0")/../tools'); sys.argv=['x']
exec(open('$(dirname "$0")/../tools/montage.py').read().split('out, k, cols')[0])
w, h, px = read_png('$cap/image_1400.png'); print(len({bytes(px[i:i+3]) for i in range(0, len(px), 4 * 97)}))" 2>/dev/null)
echo "Couleurs distinctes (échantillon) : ${colors:-0}"
[ "${colors:-0}" -gt 50 ] || { echo "ÉCHEC : capture absente ou uniforme"; fail=1; }
# Vidéo de l'écran-titre cassée (blocs vert/magenta saturés, cf. optimisations XenonRecomp du 17/11) : 0 % si correcte,
# 10-26 % si cassée ; seuil 2 % d'un échantillon de pixels.
garbage=$(python3 -I -c "
import sys; sys.path.insert(0, '$(dirname "$0")/../tools'); sys.argv=['x']
exec(open('$(dirname "$0")/../tools/montage.py').read().split('out, k, cols')[0])
w, h, px = read_png('$cap/image_1400.png'); n = bad = 0
for i in range(0, len(px), 4 * 7):
    r, g, b = px[i], px[i + 1], px[i + 2]; n += 1
    bad += (g > 180 and r < 90 and b < 90) or (r > 180 and b > 180 and g < 90)
print(int(1000 * bad / max(n, 1)))" 2>/dev/null)
echo "Blocs vert/magenta saturés : ${garbage:-?} ‰"
[ "${garbage:-1000}" -lt 20 ] || { echo "ÉCHEC : écran-titre en blocs (vidéo cassée)"; fail=1; }
[ $fail -eq 0 ] && echo "J4 (partiel) RÉUSSI"
exit $fail
