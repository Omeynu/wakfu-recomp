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
bool portableMode();                       // portable.txt à côté de l'exécutable (relu à chaque appel)
fs::path defaultWorkDir();                 // %LOCALAPPDATA%\WakfuRecomp, ou <exe>\WakfuRecomp en mode portable
fs::path appDataDir();                     // %APPDATA%\WakfuRecomp (données du mode normal ; vide si APPDATA absent)
// Mode portable : dossier de l'exécutable refusé s'il est sous Program Files ou OneDrive, non accessible en écriture
// (fichier test) ou avec moins de 3 Go libres (*gb : espace libre mesuré).
enum class RefusPortable { Aucun, ProgramFiles, OneDrive, Ecriture, Espace };
RefusPortable verifierPortable(double* gb);
void activerPortable();                    // crée portable.txt (Error si impossible)
void openWorkDir(Install& in, fs::path D); // crée D (marqueur .wakfurecomp), lit l'archive
fs::path dataDir(const fs::path& D);        // dossier des données du joueur (config.toml, sauvegardes)
bool isPrepared(const Install& in);         // jeu préparé (work/game/patched/image.bin)
bool isInstalled(const Install& in);        // jeu préparé et wakfu.exe construit
bool isUpToDate(const Install& in);         // C++ généré par cette version des outils
double freeGb(const fs::path& D);
void forceRebuild(const Install& in);       // « Recompiler » : C++ et compilation refaits

void ensureTools(Install& in);                                   // [1/5]
// [2/5] (rien si inputs est vide) ; false : jeu préparé, mais SHA-1 de l'image non reconnu (wakfu-prepare, code 3)
bool prepareGame(Install& in, const std::vector<fs::path>& inputs);
void buildGame(Install& in);                                     // [3/5] à [5/5]
int launchGame(const fs::path& D);                               // code de sortie de wakfu.exe

// Vérification rapide d'une entrée (verif.cpp) : paquet STFS, image ISO/XISO ou dossier extrait d'Islands of Wakfu.
// Dossier : dossier quelconque (ni default.xex ni default.xexp à sa racine), dont les fichiers sont à examiner.
enum class Paquet { Jeu, MiseAJour, NonStfs, AutreJeu, NiJeuNiMaj, Illisible, Tronque, MajNonPriseEnCharge, DossierMixte, Dossier };
struct InfoPaquet { uint32_t titre = 0; std::string nom, format; };   // titre et nom affiché (paquet STFS), format lu
Paquet verifierPaquet(const fs::path& p, InfoPaquet* info);

int guiMain();   // lanceur.cpp

} // namespace wk
