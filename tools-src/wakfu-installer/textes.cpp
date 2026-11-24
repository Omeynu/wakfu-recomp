// Tous les textes de la fenêtre du lanceur. %s, %d, %.1f : valeurs insérées par lanceur.cpp (même ordre en
// français et en anglais). Les messages techniques (journal, erreurs détaillées des outils) restent en français.
#include "textes.h"
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#ifdef _WIN32
#include <windows.h>
#define OS "Windows"
#else
#define OS "Linux"
#endif

namespace wk {

namespace {

struct Entree { Texte id; const char* fr; const char* en; };

const Entree kTextes[] = {
    { T_TitreFenetre, "Islands of Wakfu - WakfuRecomp", "Islands of Wakfu - WakfuRecomp" },
    { T_Marque, "ISLANDS OF WAKFU", "ISLANDS OF WAKFU" },
    { T_SousMarque, "recompilé pour " OS, "recompiled for " OS },

    { T_Bienvenue, "Bienvenue !", "Welcome!" },
    { T_AccueilIntro, "WakfuRecomp transforme votre copie Xbox 360 d'Islands of Wakfu en jeu " OS ". "
                      "Tout se passe sur votre PC : rien n'est téléchargé.",
                      "WakfuRecomp turns your Xbox 360 copy of Islands of Wakfu into a " OS " game. "
                      "Everything happens on your PC: nothing is downloaded." },
    { T_AccueilEtape1, "Choisissez votre jeu et sa mise à jour (TU 2)", "Pick your game and its title update (TU 2)" },
    { T_AccueilEtape2, "Vérification, puis recompilation : de quelques minutes à une heure",
                       "Check, then recompilation: from a few minutes to an hour" },
    { T_AccueilEtape3, "Réglez l'affichage à votre goût et jouez", "Adjust the display to your liking and play" },
    { T_AccueilNote, "Prévoyez environ 3 Go d'espace libre.", "Make sure about 3 GB of disk space is free." },
    { T_Commencer, "Commencer", "Get started" },

    { T_ChoixTitre, "Votre copie du jeu", "Your copy of the game" },
    { T_ChoixIntro, "Glissez les deux fichiers sur cette fenêtre, ou choisissez-les avec « Parcourir ».",
                    "Drag both files onto this window, or pick them with \"Browse\"." },
    { T_SlotJeu, "Jeu (Islands of Wakfu)", "Game (Islands of Wakfu)" },
    { T_SlotMaj, "Mise à jour (TU 2)", "Title update (TU 2)" },
    { T_SlotJeuAide, "Paquet Xbox Live Arcade, image ISO/XISO ou dossier extrait",
                     "Xbox Live Arcade package, ISO/XISO image or extracted folder" },
    { T_SlotMajAide, "Title Update 2 : fichier TU_... ou dossier extrait", "Title Update 2: TU_... file or extracted folder" },
    { T_AucunFichier, "Aucun fichier : glissez-le ici", "No file yet: drop it here" },
    { T_Parcourir, "Parcourir...", "Browse..." },
    { T_JeuDeBase, "Jeu de base", "Base game" },
    { T_MiseAJour2, "Mise à jour 2", "Title Update 2" },
    { T_Retour, "Retour", "Back" },
    { T_Continuer, "Continuer", "Continue" },
    { T_DialogueJeu, "Choisir le jeu (paquet Xbox Live Arcade)", "Choose the game (Xbox Live Arcade package)" },
    { T_DialogueMaj, "Choisir la mise à jour (Title Update 2)", "Choose the title update (Title Update 2)" },
    { T_FichierRefuse, "« %s » refusé : %s", "\"%s\" rejected: %s" },
    { T_ErrNonStfs, "Ce n'est pas un paquet Xbox 360 (ni une image ISO/XISO).", "This is not an Xbox 360 package (nor an ISO/XISO image)." },
    { T_ErrAutreJeu, "Autre jeu : « %s » (titre %08X).", "Another game: \"%s\" (title %08X)." },
    { T_ErrNiJeuNiMaj, "Ce paquet ne contient ni le jeu ni sa mise à jour.", "This package contains neither the game nor its update." },
    { T_ErrIllisible, "Fichier illisible ou endommagé.", "The file is unreadable or damaged." },
    { T_ErrTronque, "Fichier tronqué : la copie est incomplète.", "Truncated file: the copy is incomplete." },
    { T_ErrMajVersion, "Version de mise à jour non prise en charge (« %s ») : il faut la Title Update 2.",
                       "Unsupported title update (\"%s\"): Title Update 2 is required." },
    { T_ErrInverseJeu, "C'est le jeu, pas la mise à jour : jeu et TU inversés ?", "This is the game, not the update: game and TU swapped?" },
    { T_ErrInverseMaj, "C'est la mise à jour, pas le jeu : jeu et TU inversés ?", "This is the update, not the game: game and TU swapped?" },
    { T_ErrDossierMixte, "Ce dossier contient le jeu et la mise à jour : choisissez-les séparément.",
                         "This folder holds both the game and the update: pick them separately." },
    { T_ErrDossier, "Aucun paquet du jeu ni de la mise à jour dans ce dossier.", "No game or update package in this folder." },
    { T_ErrVersionImage, "Mauvaise version : SHA-1 de l'image non reconnu.", "Wrong version: unrecognised image SHA-1." },

    { T_VerifTitre, "Vérification de votre copie", "Checking your copy" },
    { T_VerifFichiers, "Islands of Wakfu et sa mise à jour reconnus", "Islands of Wakfu and its update recognised" },
    { T_VerifDejaPrepare, "Copie du jeu déjà préparée", "Game copy already prepared" },
    { T_VerifEspace, "%.1f Go libres sur le disque", "%.1f GB free on disk" },
    { T_VerifEspaceBas, "Seulement %.1f Go libres : environ 3 Go sont nécessaires", "Only %.1f GB free: about 3 GB are needed" },
    { T_VerifOutils, "Préparation des outils de recompilation", "Preparing the recompilation tools" },
    { T_VerifExtraction, "Extraction du jeu et application de la mise à jour", "Extracting the game and applying the update" },
    { T_VerifVersion, "Version du jeu vérifiée", "Game version verified" },
    { T_VerifOk, "Votre copie est valide.", "Your copy is valid." },
    { T_VerifOkDetail, "La recompilation peut commencer. Elle dure de quelques minutes à une heure selon votre processeur.",
                       "Recompilation can start. It takes from a few minutes to an hour depending on your processor." },
    { T_LancerRecompilation, "Lancer la recompilation", "Start recompilation" },
    { T_VerifEchec, "La vérification a échoué", "The check failed" },
    { T_MauvaiseCopieDetail, "WakfuRecomp a besoin du jeu Xbox Live Arcade et de sa mise à jour n° 2 (Title Update 2), "
                             "sans modification. Vérifiez que les deux fichiers viennent bien de votre console.",
                             "WakfuRecomp needs the unmodified Xbox Live Arcade game and its Title Update 2. "
                             "Make sure both files come from your console." },
    { T_ShaImage, "SHA-1 de l'image (jeu + mise à jour) : %s", "Image SHA-1 (game + update): %s" },
    { T_AutresFichiers, "Choisir d'autres fichiers", "Choose other files" },
    { T_OuvrirJournal, "Ouvrir le journal", "Open the log" },
    { T_Detail, "Détail :", "Details:" },

    { T_ProgTitre, "Recompilation en cours", "Recompiling" },
    { T_ProgIntro, "Vous pouvez utiliser votre PC pendant ce temps. Gardez cette fenêtre ouverte.",
                   "You can keep using your PC meanwhile. Keep this window open." },
    { T_EtOutils, "Préparation des outils", "Preparing the tools" },
    { T_EtPreparation, "Préparation de votre copie", "Preparing your copy" },
    { T_EtXenon, "Traduction du code Xbox 360", "Translating the Xbox 360 code" },
    { T_EtImports, "Liaison avec le système", "Linking with the system" },
    { T_EtConfiguration, "Configuration de la compilation", "Configuring the build" },
    { T_EtCompilation, "Compilation du jeu", "Compiling the game" },
    { T_EtInstallation, "Installation", "Installing" },
    { T_TempsRestant, "Temps restant estimé : %s", "Estimated time left: %s" },
    { T_TempsMin, "environ %d min", "about %d min" },
    { T_TempsMoinsMin, "moins d'une minute", "less than a minute" },
    { T_Calcul, "calcul en cours...", "estimating..." },
    { T_ProgEchec, "La recompilation a échoué", "Recompilation failed" },
    { T_Reessayer, "Réessayer", "Try again" },
    { T_ProgFini, "Recompilation terminée ! Réglez le jeu à votre goût, puis jouez.",
                  "Recompilation complete! Adjust the game to your liking, then play." },

    { T_Reglages, "Réglages", "Settings" },
    { T_OuvrirDossier, "Ouvrir le dossier", "Open folder" },
    { T_OngletAffichage, "Affichage", "Display" },
    { T_OngletImage, "Image", "Picture" },
    { T_OngletSonManette, "Son et manette", "Sound and controller" },
    { T_OngletClavier, "Clavier", "Keyboard" },
    { T_Enregistrer, "Enregistrer", "Save" },
    { T_Defauts, "Valeurs par défaut", "Restore defaults" },
    { T_Mode, "Mode d'affichage", "Display mode" },
    { T_ModeAide, "Plein écran sans bordure : bascule rapide vers les autres fenêtres", "Borderless: quick switching to other windows" },
    { T_ModeFenetre, "Fenêtre", "Windowed" },
    { T_ModePleinEcran, "Plein écran", "Fullscreen" },
    { T_ModeSansBordure, "Plein écran sans bordure", "Borderless fullscreen" },
    { T_Taille, "Taille de la fenêtre", "Window size" },
    { T_TailleAide, "En mode fenêtre seulement", "Windowed mode only" },
    { T_Vsync, "Synchronisation verticale", "Vertical sync" },
    { T_VsyncAide, "Évite les déchirures de l'image", "Prevents screen tearing" },
    { T_Limite, "Images par seconde au plus", "Frame rate limit" },
    { T_LimiteAide, "Réduit la chauffe et le bruit du PC", "Reduces heat and fan noise" },
    { T_Aucune, "Aucune limite", "No limit" },
    { T_AfficherIps, "Afficher les images par seconde", "Show frames per second" },
    { T_AfficherIpsAide, "Dans le titre de la fenêtre", "In the window title" },
    { T_Gpu, "Carte graphique", "Graphics card" },
    { T_GpuAide, "Une partie de son nom (ex. RX 6700) ; vide = choix automatique", "Part of its name (e.g. RX 6700); empty = automatic" },
    { T_GpuAuto, "automatique", "automatic" },
    { T_Echelle, "Définition du rendu", "Render resolution" },
    { T_EchelleAide, "Plus fine = plus nette, mais plus exigeante pour la carte graphique", "Higher = sharper, but more demanding on the graphics card" },
    { T_Anticrenelage, "Anticrénelage", "Anti-aliasing" },
    { T_AnticrenelageAide, "Adoucit les bords en escalier", "Smooths jagged edges" },
    { T_AaAucun, "Aucun", "None" },
    { T_Msaa, "Anticrénelage MSAA", "MSAA anti-aliasing" },
    { T_MsaaAide, "Auto : x4 sur carte graphique dédiée, désactivé sur GPU intégré (plus rapide) ; x8 : bords plus lisses, plus lent",
      "Auto: x4 with a dedicated graphics card, off on integrated GPUs (faster); x8: smoother edges, slower" },
    { T_MsaaAuto, "Auto", "Auto" },
    { T_Active, "Activé (x4)", "On (x4)" },
    { T_Desactive, "Désactivé", "Off" },
    { T_Agrandissement, "Agrandissement de l'image", "Image upscaling" },
    { T_AgrandissementAide, "FSR 1 : plus net en 1440p ou 4K", "FSR 1: sharper at 1440p or 4K" },
    { T_AgrLineaire, "Linéaire", "Linear" },
    { T_Nettete, "Netteté", "Sharpness" },
    { T_NetteteAide, "Renforce les détails (0 = désactivée)", "Enhances details (0 = off)" },
    { T_Luminosite, "Luminosité", "Brightness" },
    { T_LuminositeAide, "1 = image d'origine", "1 = original picture" },
    { T_Gamma, "Gamma", "Gamma" },
    { T_GammaAide, "Plus grand : tons moyens plus clairs", "Higher: brighter mid-tones" },
    { T_CouleurXbox, "Couleurs de la Xbox 360", "Xbox 360 colours" },
    { T_CouleurXboxAide, "Imite la courbe de couleurs de la console", "Mimics the console's colour curve" },
    { T_Volume, "Volume général", "Master volume" },
    { T_VolumeAide, "Musique et effets : menu du jeu", "Music and effects: in-game menu" },
    { T_Vibrations, "Vibrations de la manette", "Controller vibration" },
    { T_VibrationsAide, "0 % = aucune vibration", "0% = no vibration" },
    { T_ArrierePlan, "Manette active en arrière-plan", "Controller works in background" },
    { T_ArrierePlanAide, "Quand la fenêtre du jeu n'est pas au premier plan", "When the game window is not in front" },
    { T_PasserLogos, "Passer les logos d'introduction", "Skip intro logos" },
    { T_PasserLogosAide, "", "" },
    { T_Langue, "Langue du jeu", "Game language" },
    { T_LangueAide, "Auto : langue du système (anglais si le jeu ne l'a pas)", "Auto: system language (English if the game lacks it)" },
    { T_LangueAuto, "Auto", "Auto" },
    { T_Console, "Fenêtre du journal", "Log window" },
    { T_ConsoleAide, "Sinon, journal écrit dans journal.log", "Otherwise, the log goes to journal.log" },
    { T_ClavierAide, "Cliquez sur une commande, puis appuyez sur la touche voulue.", "Click a command, then press the key you want." },
    { T_ParDefaut, "%s (par défaut)", "%s (default)" },
    { T_AppuyerTouche, "Appuyez sur une touche...", "Press a key..." },
    { T_Reinitialiser, "Touche par défaut", "Default key" },
    { T_CmdA, "Bouton A", "A button" },
    { T_CmdB, "Bouton B", "B button" },
    { T_CmdX, "Bouton X", "X button" },
    { T_CmdY, "Bouton Y", "Y button" },
    { T_CmdStart, "START", "START" },
    { T_CmdBack, "BACK", "BACK" },
    { T_CmdLb, "Bouton LB", "LB button" },
    { T_CmdRb, "Bouton RB", "RB button" },
    { T_CmdLt, "Gâchette LT", "LT trigger" },
    { T_CmdRt, "Gâchette RT", "RT trigger" },
    { T_CmdHaut, "Croix : haut", "D-pad up" },
    { T_CmdBas, "Croix : bas", "D-pad down" },
    { T_CmdGauche, "Croix : gauche", "D-pad left" },
    { T_CmdDroite, "Croix : droite", "D-pad right" },
    { T_CmdClicG, "Clic du stick gauche", "Left stick click" },
    { T_CmdClicD, "Clic du stick droit", "Right stick click" },
    { T_CmdSgH, "Stick gauche : haut", "Left stick up" },
    { T_CmdSgB, "Stick gauche : bas", "Left stick down" },
    { T_CmdSgG, "Stick gauche : gauche", "Left stick left" },
    { T_CmdSgD, "Stick gauche : droite", "Left stick right" },
    { T_CmdSdH, "Stick droit : haut", "Right stick up" },
    { T_CmdSdB, "Stick droit : bas", "Right stick down" },
    { T_CmdSdG, "Stick droit : gauche", "Right stick left" },
    { T_CmdSdD, "Stick droit : droite", "Right stick right" },

    { T_Jouer, "Jouer", "Play" },
    { T_Recompiler, "Recompiler", "Recompile" },
    { T_SousTitreJeu, "Votre copie Xbox 360, recompilée pour " OS, "Your Xbox 360 copy, recompiled for " OS },
    { T_Donnees, "Sauvegardes et réglages : %s", "Saves and settings: %s" },
    { T_NouvelleVersion, "Nouvelle version de WakfuRecomp : recompilez le jeu pour en profiter.",
                         "New WakfuRecomp version: recompile the game to benefit from it." },
    { T_EnCours, "Le jeu est en cours...", "The game is running..." },
    { T_ConfirmRecompTitre, "Recompiler le jeu ?", "Recompile the game?" },
    { T_ConfirmRecomp, "Le jeu sera reconstruit à partir de votre copie déjà préparée (de quelques minutes à une heure). "
                       "Vos sauvegardes et réglages sont conservés.",
                       "The game will be rebuilt from your already prepared copy (a few minutes to an hour). "
                       "Your saves and settings are kept." },
    { T_Annuler, "Annuler", "Cancel" },
    { T_SortieCode, "Le jeu s'est arrêté de façon inattendue (code %d).", "The game stopped unexpectedly (code %d)." },
    { T_Sortie3, "Les fichiers du jeu semblent invalides : recompilez le jeu.", "The game files look invalid: recompile the game." },
    { T_Sortie4, "Le jeu a manqué de mémoire. Fermez d'autres programmes, puis relancez-le.",
                 "The game ran out of memory. Close other programs, then try again." },
    { T_Sortie5, "Le jeu s'est arrêté sur une erreur.", "The game stopped because of an error." },
    { T_Sortie6, "Le jeu a planté.", "The game crashed." },
    { T_VoirJournalJeu, "Voir le journal du jeu", "View the game log" },
    { T_Fermer, "Fermer", "Close" },

    { T_ErreurTitre, "Une erreur est survenue", "Something went wrong" },
    { T_ErreurDemarrage, "WakfuRecomp ne peut pas démarrer :", "WakfuRecomp cannot start:" },
    { T_QuitterTitre, "Interrompre la recompilation ?", "Stop the recompilation?" },
    { T_Quitter, "Elle reprendra où elle s'est arrêtée au prochain lancement de WakfuRecomp.",
                 "It will resume where it stopped the next time you start WakfuRecomp." },
    { T_QuitterOui, "Interrompre et quitter", "Stop and quit" },
    { T_ErrEcritureConfig, "Impossible d'enregistrer les réglages : %s", "Could not save the settings: %s" },

    { T_PasDeSelecteur, "Aucun sélecteur de fichiers (zenity ou kdialog) : glissez le fichier dans cette fenêtre.",
                        "No file picker (zenity or kdialog): drag the file into this window." },
    { T_EtapeJeu, "Jeu", "Game" },
    { T_EtapeInstallation, "Installation", "Installation" },
    { T_EtapeJouer, "Jouer", "Play" },
    { T_GarderCompilation, "Garder les fichiers de compilation", "Keep the build files" },
    { T_GarderCompilationAide, "Une prochaine recompilation sera plus rapide, mais plusieurs Go restent occupés sur le disque. "
                               "Sinon, ils sont supprimés après l'installation (le jeu, vos sauvegardes et le cache des shaders restent).",
                               "A later recompilation will be faster, but several GB stay used on the disk. Otherwise they are "
                               "deleted after the installation (the game, your saves and the shader cache stay)." },
    { T_IconeBureau, "Créer une icône sur le bureau", "Create a desktop icon" },
    { T_Desinstaller, "Désinstaller", "Uninstall" },
    { T_DesinstTitre, "Désinstaller WakfuRecomp", "Uninstall WakfuRecomp" },
    { T_DesinstIntro, "Seront supprimés :", "Will be deleted:" },
    { T_DesinstSauvegardes, "Supprimer aussi mes sauvegardes et réglages", "Also delete my saves and settings" },
    { T_DesinstGarde, "Jamais touchés : les fichiers du jeu que vous avez choisis (paquets, images) et ce lanceur.",
                      "Never touched: the game files you picked (packages, images) and this launcher." },
    { T_DesinstGardeSauv, "Gardés : vos sauvegardes et réglages (%s).", "Kept: your saves and settings (%s)." },
    { T_ConfirmSauvTitre, "Supprimer vos sauvegardes ?", "Delete your saves?" },
    { T_ConfirmSauv, "Vos sauvegardes et réglages seront définitivement supprimés.", "Your saves and settings will be permanently deleted." },
    { T_Supprimer, "Supprimer", "Delete" },
    { T_DesinstEnCours, "Suppression en cours...", "Deleting..." },
    { T_DesinstFini, "WakfuRecomp est désinstallé", "WakfuRecomp is uninstalled" },
    { T_DesinstFiniDetail, "Le lanceur n'a pas été touché : %s. Vous pouvez jeter ce fichier.",
                           "The launcher was not touched: %s. You can throw this file away." },
    { T_ErrSuppression, "La suppression n'a pas pu aller au bout : %s", "The deletion could not complete: %s" },
    { T_AnciennesTitre, "Anciennes installations trouvées", "Old installations found" },
    { T_AnciennesIntro, "Ces versions précédentes ne servent plus (rien n'en est repris). Les supprimer libère de la place.",
                        "These previous versions are no longer used (nothing is taken from them). Deleting them frees space." },
    { T_PasMaintenant, "Pas maintenant", "Not now" },
    { T_TailleGo, "%.1f Go", "%.1f GB" },
    { T_DepTitre, "Composants manquants", "Missing components" },
    { T_DepPacman, "Ils peuvent être installés avec pacman (mot de passe administrateur demandé).",
                   "They can be installed with pacman (administrator password asked)." },
    { T_DepAutre, "Installez-les avec le gestionnaire de paquets de votre distribution, puis réessayez.",
                  "Install them with your distribution's package manager, then try again." },
    { T_Installer, "Installer", "Install" },

    { T_Anisotrope, "Filtrage anisotrope", "Anisotropic filtering" },
    { T_CouperFocus, "Couper le son en arrière-plan", "Mute in background" },
    { T_CouperFocusAide, "Quand la fenêtre du jeu n'est pas au premier plan", "When the game window is not in front" },
    { T_ZoneMorte, "Zone morte des sticks", "Stick dead zone" },
    { T_ZoneMorteAide, "À augmenter si le personnage bouge seul (0 = aucune)", "Raise it if the character moves on its own (0 = none)" },
    { T_ToucheCapture, "Touche de capture d'écran", "Screenshot key" },
    { T_ToucheCaptureAide, "Images enregistrées dans le dossier captures des données", "Pictures saved in the captures folder of the data folder" },
    { T_Ecran, "Écran", "Display" },
    { T_EcranAide, "Position et taille de la fenêtre retenues à la fermeture", "Window position and size are kept when closing" },
    { T_Restaurer, "Restaurer une sauvegarde", "Restore a save" },
    { T_RestaurerTitre, "Restaurer une sauvegarde ?", "Restore a save?" },
    { T_RestaurerConfirm, "Les sauvegardes sont remplacées par celles de la copie choisie (faite au lancement du jeu). "
                          "Les sauvegardes actuelles sont d'abord copiées : rien n'est perdu.",
                          "Saves are replaced by those of the chosen copy (made when the game was launched). "
                          "Current saves are copied first: nothing is lost." },
    { T_RestaurerOui, "Restaurer", "Restore" },
    { T_AucuneCopie, "Aucune copie de secours pour l'instant (une copie est faite à chaque lancement du jeu).",
                     "No backup yet (a copy is made each time the game is launched)." },
    { T_Restauree, "Sauvegardes restaurées (copie du %s).", "Saves restored (copy of %s)." },
    { T_ErrRestauration, "Restauration impossible : %s", "Restore failed: %s" },
    { T_AnisotropeAide, "Textures vues en biais plus nettes (x1 = image d'origine)", "Sharper textures at an angle (x1 = original picture)" },
    { T_Ombres, "Définition des ombres", "Shadow resolution" },
    { T_OmbresAide, "Ombres sous les personnages plus fines, en plus de la définition du rendu (coût GPU faible)",
      "Finer shadows under characters, on top of the render resolution (small GPU cost)" },
    { T_Origine, "Origine", "Original" },
    { T_Prereglage, "Préréglage", "Preset" },
    { T_PrereglageAide, "Remplit la définition du rendu, le MSAA, le filtrage anisotrope et les ombres. "
                        "Économie : GPU intégré ; Qualité : 2560x1440 pour carte graphique dédiée",
      "Fills in render resolution, MSAA, anisotropic filtering and shadows. "
      "Economy: integrated GPU; Quality: 2560x1440 for a dedicated graphics card" },
    { T_Economie, "Économie", "Economy" },
    { T_Equilibre, "Équilibré", "Balanced" },
    { T_Qualite, "Qualité", "Quality" },
    { T_Personnalise, "Personnalisé", "Custom" },
    { T_Joueur1, "Commandes du joueur 1", "Player 1 controls" },
    { T_Joueur2, "Commandes du joueur 2 (coopératif)", "Player 2 controls (co-op)" },
    { T_JoueursAide, "Manettes dans l'ordre de branchement", "Controllers in plug-in order" },
    { T_ManetteN, "Manette %d", "Controller %d" },
};

static_assert(sizeof kTextes / sizeof kTextes[0] == NbTextes, "texte manquant ou en double");
const char* g_table[NbTextes];
bool g_fr = true;

} // namespace

void initLangue()
{
#ifdef _WIN32
    g_fr = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_FRENCH;
#else
    const char* l = nullptr;
    for (const char* v : { "LC_ALL", "LC_MESSAGES", "LANG" })
        if ((l = std::getenv(v)) && *l) break;
    g_fr = l && std::strncmp(l, "fr", 2) == 0;
#endif
    for (auto& t : g_table) t = "?";
    for (auto& e : kTextes) g_table[e.id] = g_fr ? e.fr : e.en;
}

bool langueFr() { return g_fr; }
const char* tr(Texte t) { return g_table[t]; }

} // namespace wk
