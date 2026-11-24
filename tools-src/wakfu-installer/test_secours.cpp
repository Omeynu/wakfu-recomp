// test_secours <dossier vide> : copies de secours des sauvegardes (copieSecours, restaurerSecours). Code 0 si tout passe.
// Vérifie surtout qu'aucun fichier n'est supprimé hors des copies datées de sauvegardes-secours.
#include "installer.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>
#include <string>

using namespace wk;

static int g_echecs = 0;
#define VERIFIE(c) do { if (!(c)) { std::fprintf(stderr, "ÉCHEC ligne %d : %s\n", __LINE__, #c); ++g_echecs; } } while (0)

static std::string lire(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return { std::istreambuf_iterator<char>(f), {} };
}

// Fichiers de data hors des copies datées (sauvegardes-secours/<date>/)
static std::set<std::string> horsCopies(const fs::path& data)
{
    std::set<std::string> r;
    for (auto& e : fs::recursive_directory_iterator(data))
    {
        const std::string rel = e.path().lexically_relative(data).generic_string();
        if (rel.rfind("sauvegardes-secours/20", 0) != 0) r.insert(rel);
    }
    return r;
}

int main(int argc, char** argv)
{
    if (argc != 2) { std::fprintf(stderr, "usage : test_secours <dossier vide>\n"); return 2; }
    const fs::path data = fs::absolute(argv[1]) / "donnees";
    fs::create_directories(data);
    VERIFIE(copieSecours(data).empty());   // rien à copier : aucun dossier créé
    VERIFIE(!fs::exists(data / "sauvegardes-secours"));

    fs::create_directories(data / "content" / "Single-Initiate");
    fs::create_directories(data / "profile");
    fs::create_directories(data / "captures");
    fs::create_directories(data / "sauvegardes-secours" / "a-moi");
    std::ofstream(data / "config.toml") << "cfg";
    std::ofstream(data / "journal.log") << "j";
    std::ofstream(data / "captures" / "c.png") << "png";
    std::ofstream(data / "profile" / "p.bin") << "profil";
    std::ofstream(data / "sauvegardes-secours" / "a-moi" / "garde.txt") << "x";
    std::ofstream(data / "sauvegardes-secours" / "notes.txt") << "x";
    std::ofstream(data / "sauvegardes-secours" / "2026-01-09_00-00-00.zip") << "pas un dossier";

    // 7 copies : les 5 plus récentes gardées
    for (int i = 1; i <= 7; ++i)
    {
        std::ofstream(data / "content" / "Single-Initiate" / "savegame.txt") << "partie " << i;
        const std::string nom = "2026-01-0" + std::to_string(i) + "_12-00-00";
        VERIFIE(copieSecours(data, nom) == nom);
    }
    const std::set<std::string> avant = horsCopies(data);
    std::vector<std::string> c = copiesSecours(data);
    VERIFIE(c.size() == 5 && c.front() == "2026-01-07_12-00-00" && c.back() == "2026-01-03_12-00-00");
    VERIFIE(lire(data / "sauvegardes-secours" / "2026-01-05_12-00-00" / "content" / "Single-Initiate" / "savegame.txt") == "partie 5");
    VERIFIE(lire(data / "sauvegardes-secours" / "2026-01-05_12-00-00" / "profile" / "p.bin") == "profil");
    VERIFIE(!fs::exists(data / "sauvegardes-secours" / "2026-01-05_12-00-00" / "config.toml"));   // sauvegardes seules

    // Restauration de la copie 3 (la plus ancienne gardée) : état actuel copié d'abord, fichier créé depuis gardé
    std::ofstream(data / "content" / "nouveau.txt") << "n";
    restaurerSecours(data, "2026-01-03_12-00-00");
    VERIFIE(lire(data / "content" / "Single-Initiate" / "savegame.txt") == "partie 3");
    VERIFIE(lire(data / "content" / "nouveau.txt") == "n");
    c = copiesSecours(data);
    VERIFIE(c.size() == 5 && c[1] == "2026-01-07_12-00-00");   // copie de l'état actuel en tête, la 3 élaguée ensuite
    VERIFIE(lire(data / "sauvegardes-secours" / c[0] / "content" / "Single-Initiate" / "savegame.txt") == "partie 7");

    bool refuse = false;
    try { restaurerSecours(data, "a-moi"); } catch (const Error&) { refuse = true; }
    VERIFIE(refuse);
    refuse = false;
    try { copieSecours(data, "../x"); } catch (const Error&) { refuse = true; }
    VERIFIE(refuse);

    // Aucun fichier supprimé hors des copies datées
    std::set<std::string> apres = horsCopies(data);
    apres.erase("content/nouveau.txt");
    VERIFIE(apres == avant);
    VERIFIE(lire(data / "sauvegardes-secours" / "a-moi" / "garde.txt") == "x");
    VERIFIE(lire(data / "sauvegardes-secours" / "2026-01-09_00-00-00.zip") == "pas un dossier");

    if (g_echecs) return 1;
    std::printf("test_secours : OK\n");
    return 0;
}
