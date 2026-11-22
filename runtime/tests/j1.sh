#!/bin/sh
# Jalon 1 : le jeu démarre et atteint le premier import GPU (Vd*) sans plantage ni stub en chemin.
# Usage : j1.sh <wakfu> <image.bin> <default.xex patché> [journal]
set -u
exe=$1 image=$2 xex=$3 log=${4:-j1.log}
WAKFU_STOP_AT_GPU=1 timeout 120 "$exe" "$image" "$xex" > "$log" 2>&1
rc=$?
echo "Imports appelés, dans l'ordre :"
grep '^\[premier\]' "$log" | sed 's/^\[premier\] /  /'
fail=0
[ $rc -eq 0 ] || { echo "ÉCHEC : code de sortie $rc (124 = délai dépassé)"; fail=1; }
grep -q '^\[plantage\]' "$log" && { echo "ÉCHEC : plantage"; fail=1; }
grep -q '^\[arret\] premier import GPU' "$log" || { echo "ÉCHEC : import GPU non atteint"; fail=1; }
grep -q '^\[premier\] stub' "$log" && ! grep '^\[premier\] stub' "$log" | head -1 | grep -q ' Vd' \
    && { echo "ÉCHEC : stub appelé avant le GPU"; fail=1; }
[ $fail -eq 0 ] && echo "J1 RÉUSSI : $(grep '^\[arret\]' "$log")"
exit $fail
