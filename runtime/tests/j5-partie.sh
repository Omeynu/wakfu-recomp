#!/bin/sh
# Lancement d'une partie : navigation scriptée jusqu'à « New Game », partie créée
# (journal du jeu « Start create game »), au moins un script .lua ouvert, puis au moins 300 images de jeu rendues
# et une capture finale non uniforme, puis l'image bouge (captures espacées différentes) avec un déplacement au stick. Vidéos sautées (WAKFU_SKIP_VIDEOS) tant que le décodage vidéo manque (J5).
# Usage : j5-partie.sh <wakfu> <image.bin> <xex patché> <jeu de base> <tu2> <données> [journal]
set -u
exe=$1 image=$2 xex=$3 base=$4 tu=$5 data=$6 log=${7:-j5-partie.log}
cap="${log%.log}-captures"; rm -rf "$cap"; mkdir -p "$cap"
input="15:START,25:A,32:A,39:A,46:A,53:A"
t=60; while [ $t -le 180 ]; do input="$input,$t:A"; t=$((t + 5)); done
# Déplacement au stick gauche après l'introduction (secondes ; /durée).
input="$input,200:LUP/4,210:LRIGHT/4,220:LDOWN/4,230:LLEFT/4"
WAKFU_GAMELOG=1 WAKFU_SKIP_VIDEOS=1 WAKFU_RENDER=1 WAKFU_STOP_AT_SWAP=7200 WAKFU_CAPTURE=6000,6600,7200 WAKFU_CAPTURE_DIR="$cap" \
    WAKFU_INPUT="$input" timeout 900 "$exe" "$image" "$xex" "$base" "$tu" "$data" > "$log" 2>&1
rc=$?
fail=0
[ $rc -eq 0 ] || { echo "ÉCHEC : code de sortie $rc"; fail=1; }
grep -q '^\[plantage\]' "$log" && { echo "ÉCHEC : plantage"; fail=1; }
start=$(grep -n '^\[jeu\] Start create game' "$log" | head -1 | cut -d: -f1)
[ -n "$start" ] || { echo "ÉCHEC : partie non créée"; fail=1; start=999999999; }
lua=$(grep -c 'ouvert .*\.lua ->' "$log")
echo "Scripts .lua ouverts : $lua"
[ "$lua" -gt 0 ] || { echo "ÉCHEC : aucun script .lua"; fail=1; }
f0=$(awk -v s="$start" 'NR > s && /^\[natif [0-9]+\] draws/ { gsub(/[^0-9]/, "", $2); print $2; exit }' "$log")
f1=$(grep -o '^\[arret\] [0-9]* VdSwap' "$log" | grep -o '[0-9]*' | tail -1)
echo "Première image après la création : ${f0:-?} ; dernière : ${f1:-?}"
[ -n "$f0" ] && [ -n "$f1" ] && [ $((f1 - f0)) -ge 300 ] || { echo "ÉCHEC : moins de 300 images de jeu"; fail=1; }
colors=$(python3 -I -c "
import sys; sys.argv=['x']
exec(open('$(dirname "$0")/../tools/montage.py').read().split('out, k, cols')[0])
w, h, px = read_png('$cap/image_7200.png'); print(len({bytes(px[i:i+3]) for i in range(0, len(px), 4 * 97)}))" 2>/dev/null)
echo "Couleurs distinctes (échantillon) : ${colors:-0}"
[ "${colors:-0}" -gt 50 ] || { echo "ÉCHEC : capture absente ou uniforme"; fail=1; }
# L'image bouge : captures espacées toutes différentes (personnage figé après l'introduction auparavant).
same=0; for a in 6000 6600; do b=$((a + 600)); cmp -s "$cap/image_$a.png" "$cap/image_$b.png" && same=1; done
[ $same -eq 0 ] || { echo "ÉCHEC : image figée (captures identiques)"; fail=1; }
[ $fail -eq 0 ] && echo "J5-partie RÉUSSI"
exit $fail
