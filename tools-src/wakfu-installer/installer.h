// Étapes de l'installateur, partagées par la ligne de commande (main.cpp) et la fenêtre (lanceur.cpp).
#pragma once
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace wk {

namespace fs = std::filesystem;

struct Error : std::runtime_error { using std::runtime_error::runtime_error; };

std::string u8(const fs::path& p);
fs::path fromU8(const std::string& s);

// Étapes affichées par la fenêtre (dans l'ordre)
enum Etape { EtOutils, EtPreparation, EtXenon, EtImports, EtConfiguration, EtCompilation, EtInstallation, NbEtapes };

// Suivi de la fenêtre : nullptr en ligne de commande (sorties inchangées sur la console). Appelé depuis le fil de travail.
struct Suivi
{
    virtual ~Suivi() = default;
    virtual void etape(Etape e) = 0;
    virtual void avance(double f) = 0;             // fraction de l'étape (0 à 1)
    virtual void ligne(const std::string& l) = 0;  // message de l'installateur ou sortie d'un outil (journal)
};
extern Suivi* g_suivi;

struct Install
{
    fs::path D, W, G, T, bin;   // dossier de travail, work, work/game, outils, outils/bin
    uint64_t off = 0, size = 0; // archive dans l'exécutable
    std::string id;             // empreinte de l'archive (version des outils)
};

fs::path selfPath();
fs::path defaultWorkDir();                 // %LOCALAPPDATA%\WakfuRecomp (Linux ~/.local/share/wakfu-recomp)
fs::path appDataDir();                     // %APPDATA%\WakfuRecomp (données ; vide si APPDATA absent)
void openWorkDir(Install& in, fs::path D); // crée D (marqueur .wakfurecomp), lit l'archive
fs::path dataDir(const fs::path& D);        // dossier des données du joueur (config.toml, sauvegardes)
bool isPrepared(const Install& in);         // jeu préparé (work/game/patched/image.bin)
bool isInstalled(const Install& in);        // jeu préparé et wakfu.exe construit
bool isUpToDate(const Install& in);         // jeu compilé par cette version des outils (témoin <D>/pret)
double freeGb(const fs::path& D);
void forceRebuild(const Install& in);       // « Recompiler » : C++ et compilation refaits

void ensureTools(Install& in);                                   // [1/5]
// [2/5] (rien si inputs est vide) ; false : jeu préparé, mais SHA-1 de l'image non reconnu (wakfu-prepare, code 3)
bool prepareGame(Install& in, const std::vector<fs::path>& inputs);
void buildGame(Install& in);                                     // [3/5] à [5/5]
int launchGame(const Install& in);                               // shaders natifs, puis le jeu ; code de sortie

// Vérification rapide d'une entrée (verif.cpp) : paquet STFS, image ISO/XISO ou dossier extrait d'Islands of Wakfu.
// Dossier : dossier quelconque (ni default.xex ni default.xexp à sa racine), dont les fichiers sont à examiner.
enum class Paquet { Jeu, MiseAJour, NonStfs, AutreJeu, NiJeuNiMaj, Illisible, Tronque, MajNonPriseEnCharge, DossierMixte, Dossier };
struct InfoPaquet { uint32_t titre = 0; std::string nom, format; };   // titre et nom affiché (paquet STFS), format lu
Paquet verifierPaquet(const fs::path& p, InfoPaquet* info);

// Suppression (suppression.cpp) : cible (fichier ou dossier, sans jamais suivre de lien) supprimée seulement si elle
// est racine ou dans racine ; Error si racine est vide, relative, une racine de disque, le dossier personnel ou un de
// ses parents (ou inconnu), un lien, ou si un lien se trouve entre racine et cible. Renvoie le nombre d'entrées
// supprimées ; Error si une entrée ne peut être lue ou supprimée. gardes non nul : les fichiers d'entrée (estEntree)
// sont gardés, avec les dossiers qui les contiennent, et ajoutés à *gardes.
void verifierSuppression(const fs::path& racine, const fs::path& cible);
uint64_t supprimerDans(const fs::path& racine, const fs::path& cible, std::vector<fs::path>* gardes = nullptr);
bool estEntree(const fs::path& p);   // fichier d'entrée possible : .iso/.xiso, paquet STFS (jeu, TU), image XDVDFS
uint64_t tailleSansLiens(const fs::path& p);   // octets, liens non suivis
// Ancien mode portable (suppression.cpp) : si ancien porte le marqueur .wakfurecomp et que ancien/userdata n'est pas
// vide, son contenu est copié une fois dans data (témoin data/.portable-copie) : fichiers absents seulement, rien
// n'est remplacé ni supprimé, liens non suivis. Renvoie le nombre de fichiers copiés, -1 si rien n'est à faire.
int64_t migrerPortable(const fs::path& ancien, const fs::path& data);
// Copies de secours des sauvegardes (suppression.cpp) : content et profile de data copiés dans
// data/sauvegardes-secours/<nom>/ (nom : AAAA-MM-JJ_HH-MM-SS, date locale), liens non suivis ; puis seules les 5 copies
// les plus récentes sont gardées (seuls les dossiers à ce format, dans sauvegardes-secours, sont supprimés).
// Renvoie le nom de la copie, vide s'il n'y a rien à copier ; Error si la copie échoue (rien n'est alors supprimé).
std::string copieSecours(const fs::path& data, const std::string& nom = "");
std::vector<std::string> copiesSecours(const fs::path& data);   // noms, la plus récente d'abord
// Restauration : copie de secours de l'état actuel, puis fichiers de la copie `nom` remis dans data (fichiers
// remplacés ; ceux créés depuis sont gardés, rien n'est supprimé). Error si nom n'est pas une copie.
void restaurerSecours(const fs::path& data, const std::string& nom);

// Ménage après installation : C++ recompilé, imports générés, builds CMake, outils (sauf ceux des shaders natifs,
// utilisés à chaque lancement). Renvoie les chemins supprimés.
std::vector<fs::path> menage(const Install& in);
// Désinstallation : ce qui serait supprimé (dossier d'installation sauf userdata si garder, raccourcis, données
// hors du dossier si !garder), puis suppression. Le lanceur n'est jamais touché.
std::vector<fs::path> aDesinstaller(const Install& in, bool garderSauvegardes);
void desinstaller(const Install& in, bool garderSauvegardes);
// Raccourcis : entrée de menu (Démarrer / ~/.local/share/applications), icône du bureau (facultative)
fs::path raccourciMenu();
fs::path raccourciBureau();
void creerRaccourci(const fs::path& lien, const Install& in);
// Anciennes installations (émulation, essais du rendu natif) : dossier et fichiers associés (raccourci)
struct Ancienne { fs::path dossier; std::vector<fs::path> fichiers; uint64_t taille = 0; };
std::vector<Ancienne> anciennesInstallations();
void supprimerAncienne(const Ancienne& a);
// Linux : dépendances système manquantes (commandes, bibliothèques pkg-config) ; pacman : paquets à installer
std::vector<std::string> dependancesManquantes();
bool pacmanDisponible();
void installerDependances();   // pkexec pacman -S --needed
[[noreturn]] void quitterDeSuite();   // fenêtre fermée pendant un travail : outils arrêtés avec le lanceur

int guiMain();   // lanceur.cpp

} // namespace wk
