#!/bin/sh
# Jalon 3 : GPU sans rendu. 600 images ; le processeur PM4 suit draws, resolves et shaders ; catalogue
# de shaders chargé ; aucun paquet type 3 inconnu ; capture PNG du tampon d'image valide.
# Usage : j3.sh <wakfu> <image.bin> <xex patché> <jeu de base> <tu2> <données> [journal]
set -u
exe=$1 image=$2 xex=$3 base=$4 tu=$5 data=$6 log=${7:-j3.log}
cap="${log%.log}-captures"; mkdir -p "$cap"
WAKFU_STOP_AT_SWAP=600 WAKFU_CAPTURE=600 WAKFU_CAPTURE_DIR="$cap" WAKFU_INPUT="12:START" \
    timeout 120 "$exe" "$image" "$xex" "$base" "$tu" "$data" > "$log" 2>&1
rc=$?
fail=0
[ $rc -eq 0 ] || { echo "ÉCHEC : code de sortie $rc"; fail=1; }
grep -q '^\[arret\] 600 VdSwap' "$log" || { echo "ÉCHEC : 600 VdSwap non atteints"; fail=1; }
grep -q 'catalogue : 72 shaders du jeu' "$log" || { echo "ÉCHEC : catalogue du jeu"; fail=1; }
grep -q 'catalogue : 85 shaders intégrés' "$log" || { echo "ÉCHEC : catalogue du XEX"; fail=1; }
last=$(grep '^\[image 600\]' "$log")
echo "$last"
draws=$(grep -o '^\[image [0-9]*\] draws [0-9]*' "$log" | awk '{s+=$4} END {print s+0}')
[ "$draws" -gt 0 ] || { echo "ÉCHEC : aucun draw décodé"; fail=1; }
echo "$last" | grep -q ', [1-9][0-9]* resolves$' || { echo "ÉCHEC : aucun resolve"; fail=1; }
echo "$last" | grep -q '([1-9][0-9]* du XEX' || { echo "ÉCHEC : aucun shader relié au catalogue"; fail=1; }
grep -q 'paquets type 3 inconnus' "$log" && { echo "ÉCHEC : paquets inconnus"; grep 'inconnus' "$log" | tail -1; fail=1; }
for motif in '^\[plantage\]' '^\[premier\] stub' 'cible inconnue' 'non satisfaite'; do
    grep -q "$motif" "$log" && { echo "ÉCHEC : « $motif »"; fail=1; }
done
png="$cap/image_600.png"
if [ -f "$png" ] && [ "$(head -c 8 "$png" | od -An -tx1 | tr -d ' \n')" = "89504e470d0a1a0a" ]; then
    echo "Capture : $png ($(stat -c %s "$png") octets)"
else
    echo "ÉCHEC : capture PNG absente ou invalide"; fail=1
fi
[ $fail -eq 0 ] && echo "J3 RÉUSSI"
exit $fail
