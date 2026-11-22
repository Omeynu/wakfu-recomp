#!/bin/sh
# Jalon 2 : threads, fichiers, XAM, boucle principale. Réussi si le jeu atteint 1200 VdSwap (~20 s à
# 60 Hz) en ouvrant ses fichiers, sans plantage, stub, appel indirect inconnu ni attente GPU bloquée.
# Usage : j2.sh <wakfu> <image.bin> <xex patché> <jeu de base> <tu2> <données> [journal]
set -u
exe=$1 image=$2 xex=$3 base=$4 tu=$5 data=$6 log=${7:-j2.log}
mkdir -p "$data"
WAKFU_STOP_AT_SWAP=1200 WAKFU_INPUT="12:START,14:START,16:A" timeout 120 "$exe" "$image" "$xex" "$base" "$tu" "$data" > "$log" 2>&1
rc=$?
files=$(grep -c '^\[fichier\] ouvert .*\\media\\' "$log")
fail=0
[ $rc -eq 0 ] || { echo "ÉCHEC : code de sortie $rc (124 = délai dépassé)"; fail=1; }
grep -q '^\[arret\] 1200 VdSwap' "$log" || { echo "ÉCHEC : 1200 VdSwap non atteints"; fail=1; }
[ "$files" -ge 20 ] || { echo "ÉCHEC : $files fichiers Media ouverts (20 attendus)"; fail=1; }
for motif in '^\[plantage\]' '^\[premier\] stub' 'appel indirect vers une cible inconnue' 'non satisfaite' '^\[fin\]'; do
    grep -q "$motif" "$log" && { echo "ÉCHEC : « $motif » présent"; grep "$motif" "$log" | head -3; fail=1; }
done
echo "Fichiers Media ouverts : $files ; imports distincts appelés : $(grep -c '^\[premier\]' "$log")"
[ $fail -eq 0 ] && echo "J2 RÉUSSI"
exit $fail
