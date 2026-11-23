# État du projet

Ce qui fonctionne, les performances actuelles, les problèmes rencontrés et ceux qui restent, et la suite prévue.
Pour aller plus loin : méthode de la recompilation (`methode.md`), analyse du binaire (`analyse.md`), rendu natif
(`natif.md`, `natif-plan.md`), réglages (`options.md`), imports noyau/XAM (`runtime/imports.md`).

## En bref

- Le code PowerPC du jeu (XEX + mise à jour 2) est traduit en C++ par XenonRecomp patché (15 014 fonctions), puis
  compilé sur le PC du joueur avec le runtime de ce dépôt.
- Le jeu est jouable : vidéos, menus, nouvelle partie, première zone, sauvegardes, son, manette, 6 langues.
- **Rendu natif** : les appels Direct3D du jeu sont interceptés et rendus directement en Vulkan. Il remplace
  l'émulation du GPU de la Xbox 360 utilisée au début du projet (voir « Problèmes rencontrés »).
- Windows x64 (installateur en un fichier) et Linux (paquet pour Arch/CachyOS).

## Performances actuelles

Zone 1, 1280x720. Images/s : médiane des mesures toutes les 5 s du journal du jeu (`[cadence]`) ; GPU et CPU : moyenne
par image (`[natif N] par image`).

| Machine | Images/s | Temps passé à ≥ 59,5 img/s | GPU par image | CPU du fil de rendu |
|---|---|---|---|---|
| Radeon RX 6700 XT, Linux (mesure de développement) | **60** (verrouillé, 99e centile 16,8 ms) | ≈ 100 % | 2,7 ms | 2,7 ms |
| Ryzen 7 5700U + Radeon Vega 8 (portable), CachyOS, 3 parties du 22/11 | **60** (médiane ; 57 sur une partie) | 40 à 91 % | 5,5 à 6,7 ms | 4,0 à 7,6 ms |
| Radeon RX 7900 XT, Windows, 1 partie du 22/11 | **45** (31 à 60) | 26 % | 0,7 ms | 2,6 ms |

- Le portable est passé de 40-56 à 60 img/s (GPU de 15-17 ms à ≈ 6 ms par image) avec les correctifs du 21/11 :
  mixage du son, resolves et effacements du GPU, MSAA coupé par défaut sur les GPU intégrés.
- Sous Windows, ni le GPU ni le rendu ne limitent : voir « Problèmes actuels ».
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

## Problèmes rencontrés (résolus)

**Recompilation**
- *Aucun extracteur de paquets XBLA (STFS) maintenu sous Linux* : extracteur écrit pour le projet, chaque bloc
  vérifié par SHA-1 (`scripts/stfs_extract.py`, puis `tools-src/wakfu-prepare`).
- *Tables de sauts introuvables* : XenonAnalyse n'en voyait qu'une sur 147, car le compilateur de ce jeu insère des
  `nop` qui cassent ses motifs. Détecteur par émulation : 140 automatiques et 7 relues à la main.
- *52 instructions PowerPC non traduites* (7 868 occurrences) : ajoutées à XenonRecomp
  (`patches/xenonrecomp-wakfu.patch`) et vérifiées par un test différentiel contre QEMU (`tests/insn`).
- *Horloge faussée* : l'instruction `mftb` lisait le compteur du processeur hôte (≈ 3 GHz) alors que le jeu attend
  50 MHz ; les scripts de la zone 1 restaient bloqués. Reliée à une horloge stable à 50 MHz.
- *Blocages avec le C++ optimisé* : les blocs `__finally` de MSVC lisent les registres de leur fonction parente ;
  ces 167 fonctions gardent leurs registres en mémoire.

**Noyau Xbox 360 émulé**
- *Mémoire épuisée au lancement d'une partie* : un drapeau d'allocation (`MEM_HEAP`) lu à tort comme « pages de
  16 Mo ».
- *Vidéos figées* : le compteur de temps partagé du noyau n'était jamais mis à jour.
- *Son haché et vidéos au ralenti* : le jeu a 3 ou 4 sources sonores ; leurs trames étaient mises bout à bout au
  lieu d'être additionnées, le moteur son tournait 3 à 4 fois trop lentement et les vidéos, calées sur lui, aussi.

**Rendu**
- *L'émulation du GPU plafonnait vers 30 images/s* : le processeur de commandes émulé attendait la fin du travail du
  GPU environ 9 fois par image, ce qui mettait CPU et GPU en série. Décision : intercepter Direct3D, lié
  statiquement dans le jeu et sans symboles (≈ 25 fonctions identifiées une à une), et rendre directement en
  Vulkan. Résultat : 60 images/s verrouillées et ≈ 5 400 lignes de code en moins.
- *64 shaders que XenosRecomp ne savait pas traduire* : ce sont ceux du décodeur vidéo matériel de l'XDK, que le
  jeu n'utilise pas (il décode ses vidéos sur le CPU). Rien à faire.
- *Cache de textures faux* : le jeu réécrit plusieurs fois par image la même zone mémoire ; une texture n'est
  réutilisée que si sa mémoire n'a pas changé.
- *Ombre absente sous le personnage* : défaut de l'ancienne émulation (effacement incomplet de la mémoire EDRAM) ;
  le rendu natif l'affiche correctement.

## Problèmes actuels

- **Windows plus lent que Linux** : ≈ 45 img/s sur une RX 7900 XT alors que le GPU (0,7 ms) et le fil de rendu (2,6 ms)
  sont presque inactifs. Hypothèse à vérifier : la synchro verticale émulée attend 16,7 ms avec le minuteur de
  Windows, dont la résolution par défaut est de 15,6 ms (le runtime ne demande pas de minuteur plus fin). Sous
  60 img/s, le jeu est ralenti : il avance d'un pas fixe de 1/60 s par image.
- **Portable** : 60 img/s la plupart du temps, avec des baisses ponctuelles (jusqu'à 33-40 img/s) ; CPU du fil de
  rendu jusqu'à 14 ms par image dans les scènes chargées.
- **À-coups aux chargements** : écran-titre (≈ 40 images de 130 ms), début de cinématique, passage à l'avis de
  sauvegarde (une image de 135 ms).
- **Peu d'essais sur de vrais PC Windows** (surtout Linux et Wine jusqu'ici) ; exe non signé (avertissement
  SmartScreen).
- **Sauvegarde à un point de contrôle d'origine** pas encore vérifiée en jouant (vérifiée avec un point de contrôle
  ajouté par une mise à jour de test).
- **Son des vidéos** non vérifié à l'écoute.
- **Réseau absent** : le service de jeu en ligne n'existe plus.
- **Linux** : paquet prévu pour Arch/CachyOS seulement ; pas de version macOS.

## Feuille de route

Dans l'ordre prévu. Une option nouvelle reste désactivée par défaut tant qu'elle n'est pas validée, et doit tenir
60 images/s.

1. **En cours** : cadence sous Windows (minuteur), CPU des portables, rendu en **1440p** (définition interne x1 à x4).
2. **Options rapides** : filtrage anisotrope, son coupé hors de la fenêtre, zone morte des sticks, intensité des
   vibrations, touche de capture d'écran, choix de l'écran et position de la fenêtre mémorisée, copie de secours des
   sauvegardes, préréglages (Économie / Équilibré / Qualité).
3. **Image** : niveaux de MSAA (2x, 4x, 8x), ombres en plus haute définition.
4. **Vérifications dans le jeu** : coopération locale à deux manettes (attribution des joueurs), langues des voix,
   effets plein écran à désactiver, canaux audio.
5. **Options longues** : remappage de la manette, affichage des performances en jeu, notifications de succès,
   anticrénelage de la transparence, icônes de manette Xbox/PlayStation, filtre pour daltoniens.
6. **Fin de projet** : son surround 5.1/7.1, **mods** (fichiers du jeu remplacés depuis un dossier, onglet dans le
   lanceur), **plus de 60 images/s** (logique du jeu à cadence plus haute ou images intermédiaires, à mesurer).
7. **RecompLauncher** : réglages dans le lanceur (niveau 2), puis installation complète avec barre de progression
   (niveau 3).

## Glossaire

- **XEX / TU** : exécutable Xbox 360 / mise à jour du jeu (« title update »).
- **STFS** : format des paquets Xbox Live Arcade.
- **HLE** : émulation de haut niveau, les fonctions du noyau Xbox (`Ke*`, `Nt*`, `Xam*`) réécrites en C++.
- **XAM** : bibliothèque système de la Xbox 360 (profils, manettes, sauvegardes).
- **Xenos / PM4 / EDRAM** : GPU de la Xbox 360 / format de son flux de commandes / sa mémoire vidéo intégrée.
- **XDK** : kit de développement de Microsoft ; le jeu y prend Direct3D, XUI (interface) et le lecteur vidéo.
