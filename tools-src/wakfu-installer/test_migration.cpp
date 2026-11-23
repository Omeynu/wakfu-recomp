// test_migration <dossier vide> : copie des sauvegardes de l'ancien mode portable (migrerPortable). Code 0 si tout passe.
#include "installer.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

using namespace wk;

static int g_echecs = 0;
#define VERIFIE(c) do { if (!(c)) { std::fprintf(stderr, "ÉCHEC ligne %d : %s\n", __LINE__, #c); ++g_echecs; } } while (0)

static std::string lire(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return { std::istreambuf_iterator<char>(f), {} };
}

int main(int argc, char** argv)
{
    if (argc != 2) { std::fprintf(stderr, "usage : test_migration <dossier vide>\n"); return 2; }
    const fs::path t = fs::absolute(argv[1]), ancien = t / "exe" / "WakfuRecomp", data = t / "donnees", dehors = t / "dehors";
    fs::create_directories(ancien / "userdata" / "saves" / "sous");
    fs::create_directories(data);
    fs::create_directories(dehors);
    std::ofstream(ancien / ".wakfurecomp").close();
    std::ofstream(t / "exe" / "portable.txt") << "x";
    std::ofstream(ancien / "userdata" / "config.toml") << "ancien";
    std::ofstream(ancien / "userdata" / "saves" / "s1") << "un";
    std::ofstream(ancien / "userdata" / "saves" / "sous" / "s2") << "deux";
    std::ofstream(data / "config.toml") << "nouveau";
    std::ofstream(dehors / "secret") << "dehors";
    fs::create_directory_symlink(dehors, ancien / "userdata" / "lien");

    // Copie des absents seulement, existant jamais remplacé, lien non suivi
    VERIFIE(migrerPortable(ancien, data) == 2);
    VERIFIE(lire(data / "config.toml") == "nouveau");
    VERIFIE(lire(data / "saves" / "s1") == "un");
    VERIFIE(lire(data / "saves" / "sous" / "s2") == "deux");
    VERIFIE(!fs::exists(data / "lien"));
    VERIFIE(fs::exists(data / ".portable-copie"));
    // Rien de supprimé dans l'ancien dossier ni à côté
    VERIFIE(lire(ancien / "userdata" / "config.toml") == "ancien");
    VERIFIE(lire(ancien / "userdata" / "saves" / "s1") == "un");
    VERIFIE(lire(ancien / "userdata" / "saves" / "sous" / "s2") == "deux");
    VERIFIE(fs::exists(ancien / ".wakfurecomp") && fs::exists(t / "exe" / "portable.txt") && fs::is_symlink(ancien / "userdata" / "lien"));
    VERIFIE(lire(dehors / "secret") == "dehors");

    // Une seule fois : une sauvegarde effacée par le joueur ne revient pas
    fs::remove(data / "saves" / "s1");
    VERIFIE(migrerPortable(ancien, data) == -1);
    VERIFIE(!fs::exists(data / "saves" / "s1"));

    // Dossier sans marqueur .wakfurecomp (pas un ancien dossier portable) : ignoré
    const fs::path autre = t / "autre" / "WakfuRecomp", data2 = t / "donnees2";
    fs::create_directories(autre / "userdata");
    std::ofstream(autre / "userdata" / "s") << "x";
    VERIFIE(migrerPortable(autre, data2) == -1);
    VERIFIE(!fs::exists(data2));
    // Ancien userdata vide, ou même dossier que les données : rien
    const fs::path vide = t / "vide";
    fs::create_directories(vide / "userdata");
    std::ofstream(vide / ".wakfurecomp").close();
    VERIFIE(migrerPortable(vide, data2) == -1);
    VERIFIE(migrerPortable(ancien, ancien / "userdata") == -1);
    VERIFIE(!fs::exists(ancien / "userdata" / ".portable-copie"));

    if (g_echecs) { std::fprintf(stderr, "%d échec(s)\n", g_echecs); return 1; }
    std::printf("test_migration : tout passe\n");
    return 0;
}
