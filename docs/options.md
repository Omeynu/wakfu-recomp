# Options du joueur (`config.toml`)

Le fichier `config.toml` se trouve dans le dossier des données (5e argument du runtime ; sur Windows, dossier de
données choisi par l'installateur). Il est créé au premier lancement avec les valeurs par défaut, qui reproduisent le
comportement d'origine (rendu identique). Une valeur invalide est signalée dans le journal (`[config] ...`) et
remplacée par sa valeur par défaut ; supprimer le fichier rend toutes les valeurs par défaut.
L'ancienne clé `[post] filtrage_anisotrope` (rendu par émulation, retiré en N7) est ignorée si présente ; la clé actuelle est `[video] filtrage_anisotrope`.

**Préréglages (lanceur, Réglages > Image > Préréglage)** : ils remplissent `echelle_rendu`, `multi_echantillonnage`,
`filtrage_anisotrope` et `ombres` (pas de clé propre : le préréglage affiché est déduit de ces quatre valeurs, et tout
réglage changé à la main l'affiche « Personnalisé ») ; `[post]` (anticrénelage, agrandissement) n'est pas touché.

| Préréglage | echelle_rendu | multi_echantillonnage | filtrage_anisotrope | ombres | Pourquoi (RX 6700, GPU par image, grande zone) |
|---|---|---|---|---|---|
| Auto (défauts) | 1 | `auto` | 1 | `origine` | comportement d'origine, MSAA selon le GPU |
| Économie | 1 | `desactive` | 1 | `origine` | GPU intégré (Vega 8 : 15-17 ms en zone 1) : le MSAA x4 coûte ≈ 20-35 % du GPU |
| Équilibré | 1 | `4x` | 8 | `x2` | 3,41 ms, 60 img/s p99 16,8 ms : bords lissés, coût faible sur GPU dédié |
| Qualité | 2 | `4x` | 16 | `x2` | 6,33-6,61 ms (2 parties), 60 img/s p99 16,8 ms ; zone 1 6,58 ms. MSAA x8 (9,3 ms, p99 17,4 ms) et ombres x4 (+0,95 ms) écartés |

| Section | Clé | Défaut | Effet |
|---|---|---|---|
| `[video]` | `largeur`, `hauteur` | 1280, 720 | taille de la fenêtre en mode fenêtré (320-16384 / 240-16384) |
| | `mode` | `"fenetre"` | `fenetre`, `plein_ecran` (change le mode d'affichage) ou `sans_bordure` (plein écran du bureau). Lancé depuis le mode Big Picture de RecompLauncher : `sans_bordure` pour ce lancement, sauf `plein_ecran` (fichier inchangé) |
| | `ecran` | `0` | écran de la fenêtre (index, 0 = principal) ; écran absent : principal (message `[config]`) |
| | `position_x`, `position_y` | (absentes) | position de la fenêtre dans l'écran, en pixels : mémorisées à la fermeture avec `ecran`, `largeur` et `hauteur` (mode `fenetre`, fenêtre ni agrandie ni réduite ; fenêtre fermée ou Quitter à la manette). Le jeu relit `config.toml` et le réécrit en entier (commentaires remis à neuf, autres valeurs gardées ; fichier illisible : rien n'est écrit). Absentes ou hors de l'écran : fenêtre centrée. Lanceur : changer d'écran recentre la fenêtre |
| | `vsync` | `false` | présentation synchronisée sur l'écran (FIFO) ; sinon MAILBOX ou IMMEDIATE |
| | `limite_images` | `0` | images par seconde au plus (10-1000 ; 0 = aucune limite) |
| | `afficher_images_par_seconde` | `false` | images par seconde dans le titre de la fenêtre (mise à jour chaque seconde) |
| | `multi_echantillonnage` | `"auto"` | MSAA des scènes qui le demandent (zone 1 : personnages, ombres ; le jeu demande x4). `auto` : x4 sur GPU dédié, désactivé sur GPU intégré ou logiciel ; `active` (= `4x`), `2x`, `4x`, `8x`, `desactive` : choix du joueur, toujours respecté, plafonné par le GPU (lanceur : Image > Anticrénelage MSAA). GPU et choix retenus : `journal-lanceur.txt` (`[gpu]`) et ligne `[natif]` du journal du jeu (`multi-échantillonnage xN`). Ancien fichier sans la clé : `auto`. Coût GPU par image (RX 6700, une partie de chaque, zone 1 / grande zone) : à l'échelle 1, désactivé 1,75 / 2,14 ms, x2 1,96 / 2,74, x4 2,11 / 3,32, x8 2,48 / 4,34 ; à l'échelle 2 (1440p), désactivé 5,53 / 5,81, x2 6,12 / 6,34, x4 6,32 / 6,07, x8 7,04 / 9,33 ms (x8 en 1440p : p99 17,4 ms en grande zone, 60 img/s plus tenues). |
| | `gpu` | `""` | partie du nom du GPU Vulkan voulu, casse ignorée (ex. `"RX 6700"`, `"llvmpipe"`) ; vide = GPU dédié, puis intégré, puis logiciel. Remplace `VK_ICD_FILENAMES` |
| | `echelle_rendu` | `1` | définition interne du rendu : `1` = 1280x720 (celle de la console), `2` = 2560x1440, jusqu'à `4` (5120x2880). Toutes les cibles de rendu du jeu (ombres, effets, interface) sont s fois plus fines ; le jeu n'en voit rien. L'image envoyée à la fenêtre a cette définition (puis `agrandissement` vers la fenêtre). Coût GPU et mémoire graphique : environ ×4 à l'échelle 2 |
| | `filtrage_anisotrope` | `1` | filtrage anisotrope des textures du jeu : `1` (désactivé, rendu d'origine), `2`, `4`, `8` ou `16`, plafonné par le GPU (valeur retenue : ligne `[natif]` du journal). Textures vues en biais plus nettes (le jeu, vu de haut, en a peu). Coût GPU mesuré à 16 : +0,07 ms par image (3,04 -> 3,11 ms, RX 6700, grande zone, échelle 1, 2 parties de chaque) |
| | `ombres` | `"origine"` | définition de la cible des ombres sous les personnages (masque 512x512, zone 1 et grande zone) : `origine`, `x2` ou `x4`, en plus de `echelle_rendu` (lanceur : Image > Définition des ombres). L'ombre reste au même endroit et à la même taille (captures zone 1 et grande zone, x1 et x2) ; l'ombre du jeu est floue par construction, le gain visible est faible. Coût GPU par image (RX 6700, une partie de chaque, zone 1 / grande zone) : échelle 1, origine 2,11 / 3,32 ms, x2 2,16 / -, x4 2,39 / 3,62 ; échelle 2, origine 6,33 / 6,20, x2 6,47 / -, x4 7,17 / 7,15 ms. |
| `[post]` | `agrandissement` | `"lineaire"` | `lineaire` : image du jeu (1280x720) étirée vers la fenêtre ; `fsr1` : agrandie par AMD FidelityFX FSR 1 (EASU puis netteté RCAS 0,2), plus nette en 1440p ou 4K, sans effet si la fenêtre fait 1280x720. |
| `[audio]` | `volume` | `1.0` | volume général (0 à 1) appliqué au mixage. Musique et effets : réglages du jeu (Aide et options > Réglages) |
| | `couper_hors_focus` | `false` | son coupé (volume 0) quand la fenêtre n'a pas le focus ; le mixage continue, les vidéos restent synchronisées |
| `[entrees]` | `vibrations` | `1` | intensité des vibrations de la manette, de 0 (aucune) à 1 (intensité demandée par le jeu) ; `true` / `false` (ancienne forme) valent 1 / 0 |
| | `zone_morte` | `0` | zone morte radiale des sticks de la manette, de 0 (aucune : axes transmis tels quels) à 0,5 ; la course restante est remise à l'échelle (pas de saut). Utile si le personnage avance seul (stick usé) |
| | `entree_arriere_plan` | `false` | manette lue même quand la fenêtre n'a pas le focus (le clavier exige toujours le focus) |
| | `joueur1`, `joueur2` | `"1"`, `"2"` | commandes des joueurs 1 et 2 (mode Coopératif) : `"1"` à `"4"` (manette n°, dans l'ordre de branchement ; une manette garde son numéro jusqu'à son débranchement) ou `"clavier"`. Le clavier va au joueur 1 si aucun joueur n'a `"clavier"`. Si une seule clé est donnée et prend la commande de l'autre joueur, celui-ci prend le défaut libre (`joueur2 = "1"` seul : joueur 1 sur la manette 2) ; deux valeurs identiques données : défauts gardés, message `[config]`. Le joueur 2 est connecté (profil local « Joueur 2 », réglages dans `<données>/profile2`) tant que sa manette est branchée ou qu'il a le clavier. Attribution au journal : lignes `[entrées] joueur N : …` au démarrage et à chaque branchement. Lanceur : Son et manette > Commandes du joueur 1 / 2 (choisir la commande de l'autre joueur les échange) |
| `[confort]` | `passer_logos` | `false` | les deux vidéos de logos du lancement sont sautées (le fond animé de l'écran-titre est gardé) |
| | `console` | `false` | Windows : `true` ouvre une console du journal pendant le jeu ; `false` : journal dans `<données>/journal.log`, aucune console (le lanceur crée le jeu sans console). Linux lancé par WakfuRecomp, ou Windows lancé par RecompLauncher (`WAKFU_JOURNAL=1`) : `journal.log` toujours écrit |
| | `touche_capture` | `"F12"` | touche de capture d'écran (nom de touche SDL, comme `[touches]`) : image affichée enregistrée en PNG dans `<données>/captures/capture_<date>_<image>.png` (fenêtre au premier plan) ; « Capture effectuée » (« Screenshot saved » hors français) s'affiche 2 s en haut à gauche, sans figurer sur la capture. Touche inconnue : message `[config]` au journal, pas de capture |
| `[jeu]` | `langue` | `"auto"` | langue des textes et voix du jeu : `fr`, `en`, `de`, `es`, `it`, `ja` ; `auto` : langue du système (Windows : paramètres régionaux ; Linux : `LC_ALL`, `LC_MESSAGES`, `LANG`), anglais si le jeu ne l'a pas. Lanceur : Son et confort > Langue du jeu. Valeur retenue : ligne `[langue]` du journal |
| `[touches]` | `<commande> = "<touche>"` | (aucune) | clavier : remplace les touches par défaut d'une commande. Commandes : `a b x y start back lb rb lt rt haut bas gauche droite clic_gauche clic_droit stick_gauche_{haut,bas,gauche,droite} stick_droit_{haut,bas,gauche,droite}` ; touches : noms SDL par position physique (QWERTY), ex. `"Space"`, `"Return"`, `"W"`, `"Left Shift"` |

Touches par défaut : flèches = croix, Entrée = A, Échap = B, X = X, Y = Y, Espace = START, Retour arrière / Tab =
BACK, Page préc./suiv. = LB/RB, Début/Fin = LT/RT, W A S D (Z Q S D en AZERTY) = stick gauche, I J K L = stick droit,
C / V = clic des sticks.

Quitter à la manette : maintenir BACK et START ensemble 3 secondes (le cache des pipelines est enregistré, comme à
la fermeture de la fenêtre).

Journal (option `console = false`) : `<données>/journal.log` pour le lancement en cours, `journal.1.log` et
`journal.2.log` pour les deux précédents (un ancien `journal.txt` est laissé tel quel). Dernière ligne : `[sortie] code N : raison`. Codes de sortie de
`wakfu.exe` : 0 fin normale (fenêtre fermée, le jeu quitte, Quitter à la manette) ; 2 arguments manquants ;
3 `image.bin` ou `default.xex` absents ou invalides (relancer la recompilation) ; 4 mémoire invitée (4 Go
d'adresses) impossible à réserver ; 5 erreur fatale signalée par le jeu (`KeBugCheck`) ; 6 plantage ;
7 rendu impossible (GPU sans bufferDeviceAddress, runtimeDescriptorArray ou shaderInt64, échec de Vulkan, shaders
précompilés absents) : boîte de message sous Windows, ligne `[natif]` au journal.

Dossier des données sous Windows : `%APPDATA%\WakfuRecomp` (sauvegardes, réglages du jeu, `config.toml`). Au premier
lancement par l'installateur, un ancien `<dossier de travail>\userdata` est copié (jamais supprimé).
L'ancien mode portable est retiré : si un dossier `<dossier de l'exe>\WakfuRecomp` (Linux `wakfu-recomp`) créé par
WakfuRecomp (marqueur `.wakfurecomp`) contient un `userdata` non vide, son contenu est copié une fois dans le dossier
des données (fichiers absents seulement : rien n'est remplacé ni supprimé ; témoin `.portable-copie`).
Copies de secours des sauvegardes : à chaque lancement par le lanceur, les sauvegardes (`content` et `profile` du
dossier des données) sont copiées dans `<données>/sauvegardes-secours/<AAAA-MM-JJ_HH-MM-SS>/` ; les 5 plus récentes
sont gardées. Seuls les dossiers à ce format dans `sauvegardes-secours` sont supprimés, rien d'autre. Un échec de la
copie est noté au journal du lanceur et n'empêche pas de jouer. Bouton « Restaurer une sauvegarde » (écran Jouer) :
choix de la copie, confirmation, puis copie de secours de l'état actuel et fichiers de la copie remis en place
(fichiers créés depuis gardés). Test : `test_secours`.
Les réglages de la fenêtre affichent le dossier des données et un bouton « Ouvrir le dossier ».

Tests : `runtime/tests/options.sh` (création, relecture, GPU par son nom, logos, valeur invalide ; fenêtre 800x600,
vsync et limite d'images sous Xvfb s'il est installé) et `wakfu_unit_config` (lecture, défauts, valeurs invalides,
aller-retour du fichier écrit).

## Vérifié dans le jeu (lot O3)

Parties de test sous Linux (RADV) ; les captures citées sont celles de ces parties (non publiées : elles viennent du jeu).

- **Coopératif à deux** : avant O3, non. Le jeu lit les menus par `XamInputGetKeystrokeEx` avec le drapeau
  « n'importe quel utilisateur » (r4 = `0x400000FF`) et n'acceptait que l'utilisateur 0 ; l'utilisateur 1 n'était ni
  branché ni connecté : à « Coopératif », le salon affiche « PRESS START » pour le joueur 2 et START du joueur 2 lance
  `XamShowSigninUI` sans suite (`coop4/cap/image_4000.png`). Depuis O3 (`joueur1` / `joueur2` ci-dessus, frappes
  rendues avec leur utilisateur, joueur 2 connecté quand sa commande est branchée) : le joueur 2 rejoint le salon
  (« Joueur 2 », `coop5/cap/image_4400.png`) et déplace son personnage (dragon) dans la zone 1
  (`coop6/cap/image_7800.png`, témoin sans mouvement du joueur 2 : `coop7/cap/image_8400.png`).
  Deux manettes SDL (manettes virtuelles SDL injectées) : `sdl2m/cap/image_8400.png` ;
  `joueur1 = "clavier"`, `joueur2 = "1"` (une manette au joueur 2, joueur 1 par le script d'entrées) :
  `sdlcm/cap/image_8400.png`. Non vérifié : vrai clavier (pas d'écran sur la machine de test), vraies manettes,
  vibrations du joueur 2.
- **Langues des voix** : une seule version. `Media/Sounds` contient une banque de sons (`WaveBank`, `WaveBankFx` :
  musiques et cris, ex. `FX_BOSSCRACKDEATHVOIX`) et les musiques `1.xwb` à `14.xwb`, sans dossier ni banque par
  langue ; les vidéos `Chapter1-5.wmv`, `End.wmv` ont une seule piste audio. Seuls les textes (`Media/Strings/<langue>`)
  dépendent de `langue`. Pas d'option `langue_voix`.
- **Effets plein écran** : ni flou de mouvement, ni profondeur de champ, ni bloom. Les shaders du jeu (`Media/Shaders`)
  ont des effets de niveau ou de pouvoir (`PostBeeBee`, `PostNightFire`, `PostNightStorm`, `PostRozen`, `Sepia`,
  `Prophecy`, transitions `Transition*`), un flou radial d'élément d'interface (`HudRadialBlur`) et un flou de sprite
  (`SpriteBlur`) : effets voulus du jeu, pas de réglage de confort. Pas d'option.
- **Canaux audio** : `XAudioGetSpeakerConfig` répond stéréo (`0x00010001`) ; le jeu rend toujours des trames de 6
  canaux, mais n'y met du son qu'à gauche et à droite. Mesure (ligne `[son]` du journal, crête par canal sur 10 s, exe
  Windows sous Wine avec décodage XMA, 7000 images) : G 0,27 à 0,45, D 0,27 à 0,45, centre, basses, arrière 0,00
  Flux XMA : 2 canaux (24 000 et 44 100 Hz). À refaire en répondant 5.1 (lot O5).
- **À-coup à l'échelle de rendu 2** : ce n'est pas le jeu, c'est la capture. Les mesures de O2 capturaient
  (`CAP=4000,5000`) pendant la fenêtre de mesure ; une capture relit l'image et l'encode en PNG sur le fil natif :
  ≈ 50 ms en 1280x720, 210-250 ms en 2560x1440. Même mesure sans capture (échelle 2, zone 1, images 3001-5300) :
  max 17,7 ms, aucun intervalle > 33 ms (`o3/runs/e2a`) ; avec `CAP=4000,5000` : 248 ms à l'image 4000 et 211 ms à
  l'image 5000 (`o3/runs/e2b`). Aucune correction du jeu : mesurer sans capture. La touche de capture du joueur
  (`touche_capture`) a le même coût ponctuel.
