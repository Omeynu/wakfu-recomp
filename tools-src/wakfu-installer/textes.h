// Textes de la fenêtre du lanceur (français / anglais, selon la langue du système) : tous dans textes.cpp.
#pragma once

namespace wk {

enum Texte {
    T_TitreFenetre, T_Marque, T_SousMarque,
    // Accueil
    T_Bienvenue, T_AccueilIntro, T_AccueilEtape1, T_AccueilEtape2, T_AccueilEtape3, T_AccueilNote, T_Commencer,
    // Choix des fichiers
    T_ChoixTitre, T_ChoixIntro, T_SlotJeu, T_SlotMaj, T_SlotJeuAide, T_SlotMajAide, T_AucunFichier, T_Parcourir,
    T_JeuDeBase, T_MiseAJour2, T_Retour, T_Continuer, T_DialogueJeu, T_DialogueMaj, T_FichierRefuse,
    T_ErrNonStfs, T_ErrAutreJeu, T_ErrNiJeuNiMaj, T_ErrIllisible, T_ErrTronque, T_ErrMajVersion, T_ErrInverseJeu,
    T_ErrInverseMaj, T_ErrDossierMixte, T_ErrDossier, T_ErrVersionImage,
    // Vérification
    T_VerifTitre, T_VerifFichiers, T_VerifDejaPrepare, T_VerifEspace, T_VerifEspaceBas, T_VerifOutils, T_VerifExtraction,
    T_VerifVersion, T_VerifOk, T_VerifOkDetail, T_LancerRecompilation, T_VerifEchec, T_MauvaiseCopieDetail, T_ShaImage,
    T_AutresFichiers, T_OuvrirJournal, T_Detail,
    // Progression
    T_ProgTitre, T_ProgIntro, T_EtOutils, T_EtPreparation, T_EtXenon, T_EtImports, T_EtConfiguration, T_EtCompilation,
    T_EtInstallation, T_TempsRestant, T_TempsMin, T_TempsMoinsMin, T_Calcul, T_ProgEchec, T_Reessayer, T_ProgFini,
    // Réglages
    T_Reglages, T_OuvrirDossier, T_OngletAffichage, T_OngletImage, T_OngletSonManette, T_OngletClavier, T_Enregistrer, T_Defauts,
    T_Mode, T_ModeAide, T_ModeFenetre, T_ModePleinEcran, T_ModeSansBordure, T_Taille, T_TailleAide, T_Vsync, T_VsyncAide,
    T_Limite, T_LimiteAide, T_Aucune, T_AfficherIps, T_AfficherIpsAide, T_Gpu, T_GpuAide, T_GpuAuto, T_Echelle, T_EchelleAide,
    T_Anticrenelage, T_AnticrenelageAide, T_AaAucun, T_Msaa, T_MsaaAide, T_MsaaAuto, T_Active, T_Desactive, T_Agrandissement, T_AgrandissementAide, T_AgrLineaire, T_Nettete, T_NetteteAide, T_Luminosite, T_LuminositeAide, T_Gamma, T_GammaAide,
    T_CouleurXbox, T_CouleurXboxAide, T_Volume, T_VolumeAide, T_Vibrations, T_VibrationsAide,
    T_ArrierePlan, T_ArrierePlanAide, T_PasserLogos, T_PasserLogosAide, T_Langue, T_LangueAide, T_LangueAuto, T_Console, T_ConsoleAide,
    T_ClavierAide, T_ParDefaut, T_AppuyerTouche, T_Reinitialiser,
    T_CmdA, T_CmdB, T_CmdX, T_CmdY, T_CmdStart, T_CmdBack, T_CmdLb, T_CmdRb, T_CmdLt, T_CmdRt, T_CmdHaut, T_CmdBas,
    T_CmdGauche, T_CmdDroite, T_CmdClicG, T_CmdClicD, T_CmdSgH, T_CmdSgB, T_CmdSgG, T_CmdSgD, T_CmdSdH, T_CmdSdB,
    T_CmdSdG, T_CmdSdD,
    // Jouer
    T_Jouer, T_Recompiler, T_SousTitreJeu, T_Donnees, T_NouvelleVersion, T_EnCours, T_ConfirmRecompTitre,
    T_ConfirmRecomp, T_Annuler, T_SortieCode, T_Sortie3, T_Sortie4, T_Sortie5, T_Sortie6, T_VoirJournalJeu, T_Fermer,
    // Erreurs et fermeture
    T_ErreurTitre, T_ErreurDemarrage, T_QuitterTitre, T_Quitter, T_QuitterOui, T_ErrEcritureConfig,
    // Linux, étapes, ménage, raccourcis, désinstallation, anciennes installations, dépendances
    T_PasDeSelecteur, T_EtapeJeu, T_EtapeInstallation, T_EtapeJouer, T_GarderCompilation, T_GarderCompilationAide, T_IconeBureau,
    T_Desinstaller, T_DesinstTitre, T_DesinstIntro, T_DesinstSauvegardes, T_DesinstGarde, T_DesinstGardeSauv, T_ConfirmSauvTitre,
    T_ConfirmSauv, T_Supprimer, T_DesinstEnCours, T_DesinstFini, T_DesinstFiniDetail, T_ErrSuppression, T_AnciennesTitre,
    T_AnciennesIntro, T_PasMaintenant, T_TailleGo, T_DepTitre, T_DepPacman, T_DepAutre, T_Installer,
    // Options O1
    T_Anisotrope, T_AnisotropeAide, T_CouperFocus, T_CouperFocusAide, T_ZoneMorte, T_ZoneMorteAide, T_ToucheCapture, T_ToucheCaptureAide, T_Ecran, T_EcranAide,
    T_Restaurer, T_RestaurerTitre, T_RestaurerConfirm, T_RestaurerOui, T_AucuneCopie, T_Restauree, T_ErrRestauration,
    // Options O2
    T_Ombres, T_OmbresAide, T_Origine, T_Prereglage, T_PrereglageAide, T_Economie, T_Equilibre, T_Qualite, T_Personnalise,
    // Options O3
    T_Joueur1, T_Joueur2, T_JoueursAide, T_ManetteN,
    NbTextes
};

void initLangue();          // français si la langue du système est le français (Windows : GetUserDefaultUILanguage ; Linux : LC_ALL, LC_MESSAGES, LANG)
bool langueFr();
const char* tr(Texte t);    // UTF-8

} // namespace wk
