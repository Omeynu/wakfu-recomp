#!/bin/sh
# Options du joueur (config.toml) vérifiables sans fenêtre : création au premier lancement (valeurs par défaut),
# relecture, choix du GPU par son nom, logos sautés, valeur invalide signalée et ignorée.
# Options de fenêtre (si Xvfb est installé, sinon sautées avec un message) : fenêtre 800x600, vsync (présentation FIFO)
# et limite à 10 img/s (300 images en 28 s au moins) sous un serveur X virtuel, rendu lavapipe.
# Usage : options.sh <wakfu> <image.bin> <xex patché> <jeu de base> <tu2> <données> [journal]
set -u
exe=$1 image=$2 xex=$3 base=$4 tu=$5 data=$6 log=${7:-options.log}
rm -f "$data/config.toml"
run() { WAKFU_RENDER=1 WAKFU_STOP_AT_SWAP=$2 timeout 600 "$exe" "$image" "$xex" "$base" "$tu" "$data" > "${log%.log}-$1.log" 2>&1; }
fail=0
run 1 60 || { echo "ÉCHEC : 1er lancement (code $?)"; fail=1; }
grep -q '^\[config\] .*config.toml créé' "${log%.log}-1.log" || { echo "ÉCHEC : config.toml non créé"; fail=1; }
grep -q '^largeur = 1280' "$data/config.toml" && grep -q '^passer_logos = false' "$data/config.toml" \
    || { echo "ÉCHEC : valeurs par défaut absentes du fichier"; fail=1; }
sed -i 's/^gpu = ""/gpu = "llvmpipe"/; s/^passer_logos = false/passer_logos = true/; s/^volume = 1/volume = 7/' "$data/config.toml"
run 2 1100 || { echo "ÉCHEC : 2e lancement (code $?)"; fail=1; }
grep -q '^\[config\] .*config.toml lu' "${log%.log}-2.log" || { echo "ÉCHEC : config.toml non relu"; fail=1; }
grep -q '^\[rendu\] Vulkan : llvmpipe' "${log%.log}-2.log" || { echo "ÉCHEC : GPU choisi par le nom non utilisé"; fail=1; }
logos=$(grep -c '^\[config\] logo sauté' "${log%.log}-2.log")
[ "$logos" -eq 2 ] || { echo "ÉCHEC : $logos logo(s) sauté(s) au lieu de 2"; fail=1; }
grep -q '^\[config\] \[audio\] volume : valeur invalide' "${log%.log}-2.log" || { echo "ÉCHEC : volume invalide non signalé"; fail=1; }
# Échelle de rendu interne 2 : cibles EDRAM au double du pas (1280 -> 2560), capture de l'écran-titre non uniforme
printf '[video]\nechelle_rendu = 2\n' > "$data/config.toml"
cap="${log%.log}-echelle"; rm -rf "$cap"
WAKFU_RENDER=1 WAKFU_STOP_AT_SWAP=900 WAKFU_CAPTURE=900 WAKFU_CAPTURE_DIR="$cap" timeout 600 "$exe" "$image" "$xex" "$base" "$tu" "$data" \
    > "${log%.log}-4.log" 2>&1 || { echo "ÉCHEC : lancement à l'échelle 2 (code $?)"; fail=1; }
grep -q '^\[rendu\] cible EDRAM base [0-9]* format [0-9]* pas 2560' "${log%.log}-4.log" || { echo "ÉCHEC : cibles non agrandies"; fail=1; }
[ "$(wc -c < "$cap/image_900.png" 2>/dev/null || echo 0)" -gt 100000 ] || { echo "ÉCHEC : capture à l'échelle 2 absente ou uniforme"; fail=1; }
if command -v Xvfb > /dev/null; then
    printf '[video]\nlargeur = 800\nhauteur = 600\nvsync = true\nlimite_images = 10\ngpu = "llvmpipe"\n' > "$data/config.toml"
    Xvfb :97 -screen 0 1920x1080x24 > /dev/null 2>&1 & xvfb=$!
    sleep 2
    t0=$(date +%s)
    DISPLAY=:97 WAKFU_WINDOW=1 WAKFU_RENDER=1 WAKFU_STOP_AT_SWAP=300 timeout 600 "$exe" "$image" "$xex" "$base" "$tu" "$data" \
        > "${log%.log}-3.log" 2>&1 || { echo "ÉCHEC : lancement fenêtré (code $?)"; fail=1; }
    duree=$(( $(date +%s) - t0 ))
    kill $xvfb
    grep -q '^\[fenetre\] 800x600, mode fenetre, vsync oui, limite 10' "${log%.log}-3.log" || { echo "ÉCHEC : fenêtre non configurée"; fail=1; }
    grep -q '^\[fenetre\] mode de présentation FIFO' "${log%.log}-3.log" || { echo "ÉCHEC : vsync non appliquée"; fail=1; }
    echo "300 images à 10 img/s au plus : $duree s"
    [ $duree -ge 28 ] || { echo "ÉCHEC : limite d'images non respectée"; fail=1; }
else
    echo "Options de fenêtre non testées (Xvfb absent)"
fi
rm -f "$data/config.toml"
[ $fail -eq 0 ] && echo "Options RÉUSSI"
exit $fail
