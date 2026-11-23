#!/bin/sh
# Jalon 3 : processeur de commandes sans rendu (pas de fenêtre ni WAKFU_RENDER). 600 images ; barrières, interruptions et
# swaps suivis sans paquet type 3 inconnu ni attente bloquée.
# Usage : j3.sh <wakfu> <image.bin> <xex patché> <jeu de base> <tu2> <données> [journal]
set -u
exe=$1 image=$2 xex=$3 base=$4 tu=$5 data=$6 log=${7:-j3.log}
WAKFU_STOP_AT_SWAP=600 WAKFU_INPUT="12:START" timeout 120 "$exe" "$image" "$xex" "$base" "$tu" "$data" > "$log" 2>&1
rc=$?
fail=0
[ $rc -eq 0 ] || { echo "ÉCHEC : code de sortie $rc"; fail=1; }
grep -q '^\[arret\] 600 VdSwap' "$log" || { echo "ÉCHEC : 600 VdSwap non atteints"; fail=1; }
for motif in 'paquet type 3 inconnu' '^\[plantage\]' '^\[premier\] stub' 'cible inconnue' 'non satisfaite'; do
    grep -q "$motif" "$log" && { echo "ÉCHEC : « $motif »"; grep "$motif" "$log" | head -3; fail=1; }
done
[ $fail -eq 0 ] && echo "J3 RÉUSSI"
exit $fail
