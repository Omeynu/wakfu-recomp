#!/bin/sh
# Sauvegardes persistantes (réglages de profil XamUserWrite/ReadProfileSettings -> <données>/profile).
# 1er lancement : Aide et options > Réglages, B (le jeu enregistre les valeurs par défaut, copiées), puis Réglages,
# volume de la musique baissé, B (nouvel enregistrement). 2e lancement, même dossier de données : réglages relus
# (« enregistré(s) »), Réglages puis B sans rien toucher : le jeu doit réécrire les valeurs modifiées (relues)
# et non les valeurs par défaut. Les dossiers profile/ et content/ des données sont vidés au départ.
# Partie (3e et 4e lancements) : TU de test = copie de <tu2> avec un 10100.lua qui appelle Gameplay.SaveCheckPoint()
# à la fin de l'introduction (le jeu lit update:\media\scripts avant game:) ; partie jusqu'à ce point (A toutes les
# 4 s pour passer les dialogues) -> save:\savegame.txt dans <données>/content/Single-Initiate ; puis relance avec la
# TU normale : la sauvegarde doit être relue (« result read savegame 0 ») et « Continue Game » charger la partie.
# Usage : j5-sauvegarde.sh <wakfu> <image.bin> <xex patché> <jeu de base> <tu2> <données> [journal]
set -u
exe=$1 image=$2 xex=$3 base=$4 tu=$5 data=$6 log=${7:-j5-sauvegarde.log}
rm -rf "$data/profile" "$data/content"
menu="15:START,24:DOWN,26:DOWN,28:DOWN,30:DOWN,33:A,38:DOWN,40:DOWN,43:A"   # Aide et options > Réglages
run() {   # run <n> <entrées>
    WAKFU_SKIP_VIDEOS=1 WAKFU_RENDER=1 WAKFU_STOP_AT_SWAP=4800 WAKFU_INPUT="$2" \
        timeout 600 "$exe" "$image" "$xex" "$base" "$tu" "$data" > "${log%.log}-$1.log" 2>&1
}
sum() { cat "$data"/profile/*.bin 2>/dev/null | cksum; }
fail=0
run 0 "$menu,47:B" || { echo "ÉCHEC : lancement 0 (code $?)"; fail=1; }
defaut=$(sum)
run 1 "$menu,47:LEFT,48:LEFT,49:LEFT,50:LEFT,51:LEFT,52:LEFT,55:B" || { echo "ÉCHEC : 1er lancement (code $?)"; fail=1; }
grep -q '^\[profil\] écriture de [1-9][0-9]*/' "${log%.log}-1.log" || { echo "ÉCHEC : réglages non écrits"; fail=1; }
echo "Réglages enregistrés : $(ls "$data/profile" 2>/dev/null | grep -c '\.bin$')"
modifie=$(sum)
[ "$modifie" != "$defaut" ] || { echo "ÉCHEC : la modification n'a pas changé les réglages enregistrés"; fail=1; }
run 2 "$menu,47:B" || { echo "ÉCHEC : 2e lancement (code $?)"; fail=1; }
grep -q '^\[profil\] lecture de [0-9]* réglage(s) ([1-9][0-9]* enregistré' "${log%.log}-2.log" \
    || { echo "ÉCHEC : réglages non relus"; fail=1; }
grep -q '^\[profil\] écriture' "${log%.log}-2.log" || { echo "ÉCHEC : pas de réécriture au 2e lancement"; fail=1; }
[ "$(sum)" = "$modifie" ] || { echo "ÉCHEC : réglages du 2e lancement différents de ceux enregistrés"; fail=1; }
# Partie : point de contrôle puis rechargement
tutest="${log%.log}-tu"; rm -rf "$tutest"; cp -r "$tu" "$tutest"; mkdir -p "$tutest/Media/Scripts"
awk '{ l[NR] = $0 } /^end/ { e = NR } END { for (i = 1; i <= NR; i++) { if (i == e) print "    Gameplay.SaveCheckPoint()"; print l[i] } }' \
    "$base/Media/Scripts/10100.lua" > "$tutest/Media/Scripts/10100.lua"
grep -q 'SaveCheckPoint' "$tutest/Media/Scripts/10100.lua" || { echo "ÉCHEC : TU de test non préparée"; fail=1; }
partie="15:START,25:A,32:A,39:A,46:A,53:A"; t=60; while [ $t -le 400 ]; do partie="$partie,$t:A"; t=$((t + 4)); done
WAKFU_GAMELOG=1 WAKFU_SKIP_VIDEOS=1 WAKFU_RENDER=1 WAKFU_STOP_AT_SWAP=10000 WAKFU_INPUT="$partie" \
    timeout 1500 "$exe" "$image" "$xex" "$base" "$tutest" "$data" > "${log%.log}-3.log" 2>&1 || { echo "ÉCHEC : 3e lancement (code $?)"; fail=1; }
save="$data/content/Single-Initiate/savegame.txt"
grep -q '^\[contenu\] save: -> Single-Initiate, drapeaux 2 : créé' "${log%.log}-3.log" || { echo "ÉCHEC : contenu de sauvegarde non créé"; fail=1; }
[ -s "$save" ] && echo "Partie enregistrée : $(wc -c < "$save") octets" || { echo "ÉCHEC : savegame.txt absent ou vide"; fail=1; }
WAKFU_GAMELOG=1 WAKFU_SKIP_VIDEOS=1 WAKFU_RENDER=1 WAKFU_STOP_AT_SWAP=3000 WAKFU_INPUT="15:START,25:A,32:A,39:A,46:A,53:A" \
    timeout 600 "$exe" "$image" "$xex" "$base" "$tu" "$data" > "${log%.log}-4.log" 2>&1 || { echo "ÉCHEC : 4e lancement (code $?)"; fail=1; }
grep -q '^\[jeu\] result read savegame 0' "${log%.log}-4.log" || { echo "ÉCHEC : sauvegarde de partie non relue"; fail=1; }
grep -q '^\[jeu\] Start create game' "${log%.log}-4.log" || { echo "ÉCHEC : partie non rechargée"; fail=1; }
rm -rf "$tutest"
[ $fail -eq 0 ] && echo "J5-sauvegarde RÉUSSI"
exit $fail
