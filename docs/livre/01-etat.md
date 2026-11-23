# Chapitre 1 — État du projet

[Sommaire](README.md) · Suivant : [Problèmes et solutions](problemes/README.md)

## En bref

- Le code PowerPC du jeu (XEX + mise à jour 2) est traduit en C++ par XenonRecomp patché (15 014 fonctions), puis
  compilé sur le PC du joueur avec le runtime de ce dépôt.
- Le jeu est jouable : vidéos, menus, nouvelle partie, première zone, sauvegardes, son, manette, 6 langues.
- **Rendu natif** : les appels Direct3D du jeu sont interceptés et rendus directement en Vulkan. Il remplace
  l'émulation du GPU de la Xbox 360 utilisée au début du projet (voir la fiche [L'émulation du GPU plafonnait](problemes/p09-emulation-gpu.md)).
- Windows x64 (installateur en un fichier) et Linux (paquet pour Arch/CachyOS).

## Performances actuelles

Zone 1, 1280x720. Images/s : médiane des mesures toutes les 5 s du journal du jeu (`[cadence]`) ; GPU et CPU : moyenne
par image (`[natif N] par image`).

| Machine | Images/s | Temps passé à ≥ 59,5 img/s | GPU par image | CPU du fil de rendu |
|---|---|---|---|---|
| Radeon RX 6700 XT, Linux (mesure de développement) | **60** (verrouillé, 99e centile 16,8 ms) | ≈ 100 % | 2,7 ms | 2,7 ms |
| Ryzen 7 5700U + Radeon Vega 8 (portable), CachyOS, 3 parties du 22/11 | **60** (médiane ; 57 sur une partie) | 40 à 91 % | 5,5 à 6,7 ms | 4,0 à 7,6 ms |
| Radeon RX 7900 XT, Windows, 2 parties des 22 et 23/11 | **45** puis **59,4** (31 à 60) | 26 % puis 48 % | 0,7 à 1,6 ms | 2,6 à 5 ms |

- Le portable est passé de 40-56 à 60 img/s (GPU de 15-17 ms à ≈ 6 ms par image) avec les correctifs du 21/11 :
  mixage du son, resolves et effacements du GPU, MSAA coupé par défaut sur les GPU intégrés.
- Sous Windows, ni le GPU ni le rendu ne limitent : voir la fiche [Chutes de cadence sous Windows](problemes/p12-cadence-windows.md).
- Menus : 59 img/s. L'écran d'avis de sauvegarde tourne à 30 img/s : c'est voulu par le jeu.
- Pour comparaison, avec l'ancienne émulation du GPU, la même scène tournait entre 6 et 30 img/s selon les versions,
  45-55 au mieux.

## Jalons

Les commentaires du code y font référence (« jalon 1 », « J4 », « N7 »…).

| Jalon | Contenu | État |
|---|---|---|
| J1 à J3 | démarrage, noyau émulé, processeur de commandes du GPU | fait (`runtime/tests/j1.sh` à `j3.sh`) |
| J4 | rendu Vulkan, menus | fait (`j4.sh`) |
| J5 | son, vidéos, manette, sauvegardes, partie lancée | fait (`j5-partie.sh`, `j5-sauvegarde.sh`) |
| J6 | installateur Windows, paquet Linux | fait |
| N0 à N7 | rendu natif par interception de Direct3D, puis retrait de l'émulation du GPU | fait (`natif.md`) |
| N8 | GPU intégrés (portable), son, vidéos | fait en partie : CPU du portable en cours |
| N9 | lanceur unique Windows + Linux, désinstallation | fait |
| N10 | rendu en 1440p (définition interne x1 à x4) | en cours |
