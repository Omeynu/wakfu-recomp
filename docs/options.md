# Options du joueur (`config.toml`)

Le fichier `config.toml` se trouve dans le dossier des données (5e argument du runtime ; sur Windows, dossier de
données choisi par l'installateur). Il est créé au premier lancement avec les valeurs par défaut, qui reproduisent le
comportement d'origine (rendu identique). Une valeur invalide est signalée dans le journal (`[config] ...`) et
remplacée par sa valeur par défaut ; supprimer le fichier rend toutes les valeurs par défaut.
`echelle_rendu` et `filtrage_anisotrope` (rendu par émulation) n'existent plus depuis N7 : ignorées si présentes.

| Section | Clé | Défaut | Effet |
|---|---|---|---|
| `[video]` | `largeur`, `hauteur` | 1280, 720 | taille de la fenêtre en mode fenêtré (320-16384 / 240-16384) |
| | `mode` | `"fenetre"` | `fenetre`, `plein_ecran` (change le mode d'affichage) ou `sans_bordure` (plein écran du bureau) |
| | `vsync` | `false` | présentation synchronisée sur l'écran (FIFO) ; sinon MAILBOX ou IMMEDIATE |
| | `limite_images` | `0` | images par seconde au plus (10-1000 ; 0 = aucune limite) |
| | `afficher_images_par_seconde` | `false` | images par seconde dans le titre de la fenêtre (mise à jour chaque seconde) |
| | `multi_echantillonnage` | `"auto"` | MSAA x4 des scènes qui le demandent (zone 1 : personnages, ombres). `auto` : activé sur GPU dédié, désactivé sur GPU intégré ou logiciel ; `active` / `desactive` : choix du joueur, toujours respecté (lanceur : Image > Anticrénelage MSAA). Désactivé : GPU ≈ 20 % plus rapide (RX 6700, zone 1), bords de ces objets un peu crénelés. GPU et choix retenus : `journal-lanceur.txt` (`[gpu]`) et ligne `[natif]` du journal du jeu. Ancien fichier sans la clé : `auto`. |
| | `gpu` | `""` | partie du nom du GPU Vulkan voulu, casse ignorée (ex. `"RX 6700"`, `"llvmpipe"`) ; vide = GPU dédié, puis intégré, puis logiciel. Remplace `VK_ICD_FILENAMES` |
| `[post]` | `agrandissement` | `"lineaire"` | `lineaire` : image du jeu (1280x720) étirée vers la fenêtre ; `fsr1` : agrandie par AMD FidelityFX FSR 1 (EASU puis netteté RCAS 0,2), plus nette en 1440p ou 4K, sans effet si la fenêtre fait 1280x720. |
| `[audio]` | `volume` | `1.0` | volume général (0 à 1) appliqué au mixage. Musique et effets : réglages du jeu (Aide et options > Réglages) |
| `[entrees]` | `vibrations` | `true` | vibrations de la manette |
| | `entree_arriere_plan` | `false` | manette lue même quand la fenêtre n'a pas le focus (le clavier exige toujours le focus) |
| `[confort]` | `passer_logos` | `false` | les deux vidéos de logos du lancement sont sautées (le fond animé de l'écran-titre est gardé) |
| | `console` | `true` | Windows : console du journal ; `false` : journal dans `<données>/journal.txt`, pas de console. Linux lancé par WakfuRecomp (`WAKFU_JOURNAL=1`) : `journal.txt` toujours écrit |
| `[jeu]` | `langue` | `"auto"` | langue des textes et voix du jeu : `fr`, `en`, `de`, `es`, `it`, `ja` ; `auto` : langue du système (Windows : paramètres régionaux ; Linux : `LC_ALL`, `LC_MESSAGES`, `LANG`), anglais si le jeu ne l'a pas. Lanceur : Son et confort > Langue du jeu. Valeur retenue : ligne `[langue]` du journal |
| `[touches]` | `<commande> = "<touche>"` | (aucune) | clavier : remplace les touches par défaut d'une commande. Commandes : `a b x y start back lb rb lt rt haut bas gauche droite clic_gauche clic_droit stick_gauche_{haut,bas,gauche,droite} stick_droit_{haut,bas,gauche,droite}` ; touches : noms SDL par position physique (QWERTY), ex. `"Space"`, `"Return"`, `"W"`, `"Left Shift"` |

Touches par défaut : flèches = croix, Entrée = A, Échap = B, X = X, Y = Y, Espace = START, Retour arrière / Tab =
BACK, Page préc./suiv. = LB/RB, Début/Fin = LT/RT, W A S D (Z Q S D en AZERTY) = stick gauche, I J K L = stick droit,
C / V = clic des sticks.

Quitter à la manette : maintenir BACK et START ensemble 3 secondes (le cache des pipelines est enregistré, comme à
la fermeture de la fenêtre).

Journal (option `console = false`) : `<données>/journal.txt` pour le lancement en cours, `journal.1.txt` et
`journal.2.txt` pour les deux précédents. Dernière ligne : `[sortie] code N : raison`. Codes de sortie de
`wakfu.exe` : 0 fin normale (fenêtre fermée, le jeu quitte, Quitter à la manette) ; 2 arguments manquants ;
3 `image.bin` ou `default.xex` absents ou invalides (relancer la recompilation) ; 4 mémoire invitée (4 Go
d'adresses) impossible à réserver ; 5 erreur fatale signalée par le jeu (`KeBugCheck`) ; 6 plantage ;
7 rendu impossible (GPU sans bufferDeviceAddress, runtimeDescriptorArray ou shaderInt64, échec de Vulkan, shaders
précompilés absents) : boîte de message sous Windows, ligne `[natif]` au journal.

Dossier des données sous Windows : `%APPDATA%\WakfuRecomp` (sauvegardes, réglages du jeu, `config.toml`). Au premier
lancement par l'installateur, un ancien `<dossier de travail>\userdata` est copié (jamais supprimé).
Mode portable : un fichier `portable.txt` (contenu indifférent) à côté de `WakfuRecomp.exe` place le dossier de
travail dans `<dossier de l'exe>\WakfuRecomp` et les données dans `<dossier de l'exe>\WakfuRecomp\userdata`.
Au premier lancement de la fenêtre, l'accueil propose les deux modes (« Installation normale », recommandée, ou
« Portable ») et crée `portable.txt` si le mode portable est choisi. Il est refusé (retour au mode normal, avec la
raison) si le dossier de l'exe est sous Program Files ou dans un dossier OneDrive (variables `OneDrive`,
`OneDriveConsumer`, `OneDriveCommercial`), n'est pas accessible en écriture (fichier test) ou a moins de 3 Go libres.
Si `%APPDATA%\WakfuRecomp` contient déjà des fichiers, la case « Copier mes sauvegardes existantes » (cochée) les copie
dans `userdata` ; rien n'est supprimé, et un fichier déjà présent n'est remplacé qu'après confirmation. Les réglages
de la fenêtre affichent le mode, le dossier des données et un bouton « Ouvrir le dossier ».

Tests : `runtime/tests/options.sh` (création, relecture, GPU par son nom, logos, valeur invalide ; fenêtre 800x600,
vsync et limite d'images sous Xvfb s'il est installé) et `wakfu_unit_config` (lecture, défauts, valeurs invalides,
aller-retour du fichier écrit).
