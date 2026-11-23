// test_suppression <dossier vide> : garde de supprimerDans (refus, liens jamais suivis). Code 0 si tout passe.
#include "installer.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace wk;

static int g_echecs = 0;
#define VERIFIE(c) do { if (!(c)) { std::fprintf(stderr, "ÉCHEC ligne %d : %s\n", __LINE__, #c); ++g_echecs; } } while (0)

static bool refuse(const fs::path& r, const fs::path& c)
{
    try { verifierSuppression(r, c); return false; }
    catch (const Error&) { return true; }
}

int main(int argc, char** argv)
{
    if (argc != 2) { std::fprintf(stderr, "usage : test_suppression <dossier vide>\n"); return 2; }
    const fs::path t = fs::absolute(argv[1]), racine = t / "inst", dehors = t / "dehors";
    fs::create_directories(racine / "a" / "b");
    fs::create_directories(dehors / "d");
    std::ofstream(racine / "a" / "b" / "f") << "x";
    std::ofstream(dehors / "g") << "garder";
    std::ofstream(dehors / "d" / "h") << "garder";
    fs::create_directory_symlink(dehors, racine / "lien-dossier");
    fs::create_symlink(dehors / "g", racine / "a" / "lien-fichier");
    fs::create_directory_symlink(racine, t / "lien-racine");

    // Refus
    VERIFIE(refuse("", racine / "a"));
    VERIFIE(refuse("relatif", "relatif/a"));
    VERIFIE(refuse(racine.root_path(), racine));
    if (const char* h = std::getenv("HOME"))
    {
        VERIFIE(refuse(h, fs::path(h) / "x"));
        VERIFIE(refuse(fs::path(h).parent_path(), fs::path(h).parent_path() / "x"));
    }
    VERIFIE(refuse(racine, dehors / "g"));                     // hors de la racine
    VERIFIE(refuse(racine, racine / ".." / "dehors"));         // .. qui sort
    VERIFIE(refuse(racine, racine / "lien-dossier" / "g"));    // à travers un lien
    VERIFIE(refuse(t / "lien-racine", t / "lien-racine" / "a"));   // racine qui est un lien
    VERIFIE(!refuse(racine, racine / "a" / "b"));
    VERIFIE(!refuse(racine, racine));
#ifdef _WIN32
    VERIFIE(!refuse(racine, t / "INST" / "a"));   // casse ignorée
    VERIFIE(refuse(racine, t / "INSTX" / "a"));
#else
    VERIFIE(refuse(racine, t / "INST" / "a"));    // casse significative
#endif
    {   // HOME (USERPROFILE) absent : refus (le contrôle n'est pas désactivé)
#ifdef _WIN32
        const char* v = "USERPROFILE";
        auto poser = [&](const char* x) { _putenv_s(v, x ? x : ""); };
#else
        const char* v = "HOME";
        auto poser = [&](const char* x) { x ? setenv(v, x, 1) : unsetenv(v); };
#endif
        std::string h = std::getenv(v) ? std::getenv(v) : "";
        poser(nullptr);
        VERIFIE(refuse(racine, racine / "a"));
        if (!h.empty()) poser(h.c_str());
    }

    // Fichiers d'entrée gardés (désinstallation) : .iso (casse ignorée), STFS (LIVE), image XDVDFS ; le reste supprimé
    fs::create_directories(racine / "e" / "s");
    std::ofstream(racine / "e" / "jeu.ISO") << "x";
    std::ofstream(racine / "e" / "s" / "TU_1C422DA") << "LIVE....";
    std::ofstream(racine / "e" / "s" / "autre") << "rien";
    { std::ofstream x(racine / "e" / "image", std::ios::binary); x.seekp(32 * 2048); x << "MICROSOFT*XBOX*MEDIA"; }
    std::ofstream(racine / "e" / "vide") << "";
    std::vector<fs::path> gardes;
    VERIFIE(supprimerDans(racine, racine / "e", &gardes) == 2);   // autre, vide
    VERIFIE(gardes.size() == 3 && fs::exists(racine / "e" / "jeu.ISO") && fs::exists(racine / "e" / "s" / "TU_1C422DA") && fs::exists(racine / "e" / "image"));
    VERIFIE(!fs::exists(racine / "e" / "s" / "autre") && !fs::exists(racine / "e" / "vide"));
    VERIFIE(supprimerDans(racine, racine / "e") > 0 && !fs::exists(racine / "e"));   // sans gardes : tout part

    // Rien à supprimer : 0, sans erreur
    VERIFIE(supprimerDans(racine, racine / "absent") == 0);
    // Un lien est supprimé seul, sa cible reste
    VERIFIE(supprimerDans(racine, racine / "lien-dossier") == 1);
    VERIFIE(fs::exists(dehors / "d" / "h"));
    // Tout le dossier : liens internes non suivis
    VERIFIE(supprimerDans(racine, racine) > 0);
    VERIFIE(!fs::exists(fs::symlink_status(racine)));
    VERIFIE(fs::exists(dehors / "g") && fs::exists(dehors / "d" / "h"));
    VERIFIE(tailleSansLiens(dehors) == 12);

    std::printf(g_echecs ? "test_suppression : %d échec(s)\n" : "test_suppression : OK\n", g_echecs);
    return g_echecs ? 1 : 0;
}
