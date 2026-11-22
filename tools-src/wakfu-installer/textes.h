// Textes de la fenêtre du lanceur (français / anglais, selon la langue de Windows) : tous dans textes.cpp.
#pragma once

namespace wk {

enum Texte {
    T_TitreFenetre, T_Marque, T_SousMarque,
    // Accueil
    T_Bienvenue, T_AccueilIntro, T_AccueilEtape1, T_AccueilEtape2, T_AccueilEtape3, T_AccueilNote, T_Commencer,
    T_OuInstaller, T_InstNormale, T_Recommande, T_InstNormaleAide, T_InstPortable, T_InstPortableAide, T_CopierSauvegardes,
    T_PortableProgramFiles, T_PortableOneDrive, T_PortableEcriture, T_PortableEspace, T_EcraserTitre, T_Ecraser, T_Remplacer,
    T_ErrCopieSauvegardes,
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
    T_Reglages, T_ModeInstallation, T_OuvrirDossier, T_OngletAffichage, T_OngletImage, T_OngletSonManette, T_OngletClavier, T_Enregistrer, T_Defauts,
    T_Mode, T_ModeAide, T_ModeFenetre, T_ModePleinEcran, T_ModeSansBordure, T_Taille, T_TailleAide, T_Vsync, T_VsyncAide,
    T_Limite, T_LimiteAide, T_Aucune, T_AfficherIps, T_AfficherIpsAide, T_Gpu, T_GpuAide, T_GpuAuto, T_Echelle, T_EchelleAide,
    T_Anticrenelage, T_AnticrenelageAide, T_AaAucun, T_Agrandissement, T_AgrandissementAide, T_AgrLineaire, T_Nettete, T_NetteteAide, T_Luminosite, T_LuminositeAide, T_Gamma, T_GammaAide,
    T_CouleurXbox, T_CouleurXboxAide, T_Anisotrope, T_AnisotropeAide, T_Volume, T_VolumeAide, T_Vibrations, T_VibrationsAide,
    T_ArrierePlan, T_ArrierePlanAide, T_PasserLogos, T_PasserLogosAide, T_Console, T_ConsoleAide,
    T_ClavierAide, T_ParDefaut, T_AppuyerTouche, T_Reinitialiser, T_Desactive,
    T_CmdA, T_CmdB, T_CmdX, T_CmdY, T_CmdStart, T_CmdBack, T_CmdLb, T_CmdRb, T_CmdLt, T_CmdRt, T_CmdHaut, T_CmdBas,
    T_CmdGauche, T_CmdDroite, T_CmdClicG, T_CmdClicD, T_CmdSgH, T_CmdSgB, T_CmdSgG, T_CmdSgD, T_CmdSdH, T_CmdSdB,
    T_CmdSdG, T_CmdSdD,
    // Jouer
    T_Jouer, T_Recompiler, T_SousTitreJeu, T_Donnees, T_NouvelleVersion, T_EnCours, T_ConfirmRecompTitre,
    T_ConfirmRecomp, T_Annuler, T_SortieCode, T_Sortie3, T_Sortie4, T_Sortie5, T_Sortie6, T_VoirJournalJeu, T_Fermer,
    // Erreurs et fermeture
    T_ErreurTitre, T_ErreurDemarrage, T_QuitterTitre, T_Quitter, T_QuitterOui, T_ErrEcritureConfig,
    NbTextes
};

void initLangue();          // GetUserDefaultUILanguage : français si la langue de Windows est le français
bool langueFr();
const char* tr(Texte t);    // UTF-8

} // namespace wk
