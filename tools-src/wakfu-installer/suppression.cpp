// Seule fonction de suppression récursive du lanceur (ménage, désinstallation, anciennes installations) : bornée à
// un dossier racine vérifié, ne suit jamais de lien symbolique (ni de jonction sous Windows). Copie des sauvegardes
// de l'ancien mode portable (sans rien remplacer ni supprimer).
#ifdef _WIN32
#include <windows.h>
#endif
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include "installer.h"
#include "xiso.h"

namespace wk {

std::string u8(const fs::path& p) { auto s = p.generic_u8string(); return std::string(s.begin(), s.end()); }
fs::path fromU8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

namespace {

// Lien symbolique, ou point d'analyse Windows (jonction, lien OneDrive...). Type illisible : traité comme un lien
// (refus, jamais de descente). Chemin absent : pas un lien.
bool estLien(const fs::path& p)
{
    std::error_code ec;
    auto s = fs::symlink_status(p, ec);
    if (s.type() == fs::file_type::not_found) return false;
    if (ec || fs::is_symlink(s)) return true;
#ifdef _WIN32
    DWORD a = GetFileAttributesW(p.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
#endif
    return false;
}

fs::path dossierPerso()
{
#ifdef _WIN32
    const char* h = std::getenv("USERPROFILE");
#else
    const char* h = std::getenv("HOME");
#endif
    std::error_code ec;
    return h && *h ? fs::weakly_canonical(fs::path(h), ec).lexically_normal() : fs::path();
}

fs::path cle(const fs::path& p)   // forme comparable : Windows ignore la casse
{
#ifdef _WIN32
    std::wstring s = p.wstring();
    CharLowerBuffW(s.data(), DWORD(s.size()));
    return fs::path(s);
#else
    return p;
#endif
}

fs::path sansSeparateurFinal(fs::path p)
{
    p = p.lexically_normal();
    if (!p.has_filename() && p != p.root_path()) p = p.parent_path();
    return p;
}

// Renvoie false si p est resté (fichier d'entrée gardé, ou dossier qui en contient un)
bool effacer(const fs::path& p, uint64_t& n, std::vector<fs::path>* gardes)
{
    std::error_code ec;
    bool vide = true;
    if (!estLien(p) && fs::is_directory(fs::symlink_status(p, ec)))
    {
        std::vector<fs::path> l;   // listé d'abord : rien n'est supprimé pendant l'itération
        for (auto it = fs::directory_iterator(p, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) l.push_back(it->path());
        if (ec) throw Error("suppression : lecture de " + u8(p) + " impossible : " + ec.message());
        for (auto& e : l) vide &= effacer(e, n, gardes);
    }
    else if (gardes && estEntree(p))
    {
        gardes->push_back(p);
        return false;
    }
    if (!vide) return false;
    fs::remove(p, ec);   // un lien est supprimé lui-même, jamais sa cible
    if (ec) throw Error("suppression de " + u8(p) + " impossible : " + ec.message());
    ++n;
    return true;
}

} // namespace

void verifierSuppression(const fs::path& racine, const fs::path& cible)
{
    if (racine.empty() || !racine.is_absolute()) throw Error("suppression refusée : dossier racine vide ou relatif");
    const fs::path r = sansSeparateurFinal(racine), c = sansSeparateurFinal(cible), h = dossierPerso();
    const auto dans = [](const fs::path& p, const fs::path& d) {   // p est d ou dans d (chemins normalisés)
        fs::path rel = cle(p).lexically_relative(cle(d));
        return !rel.empty() && *rel.begin() != "..";
    };
    if (r == r.root_path()) throw Error("suppression refusée : racine du disque " + u8(r));
    if (h.empty()) throw Error("suppression refusée : dossier personnel inconnu (HOME / USERPROFILE absent)");
    if (dans(h, r)) throw Error("suppression refusée : " + u8(r) + " contient le dossier personnel");
    if (!dans(c, r)) throw Error("suppression refusée : " + u8(c) + " hors de " + u8(r));
    if (estLien(r)) throw Error("suppression refusée : " + u8(r) + " est un lien");
    fs::path p = r;   // aucun lien entre la racine et la cible (la cible elle-même peut en être un : il est supprimé seul)
    const fs::path rel = cle(c).lexically_relative(cle(r));
    for (auto it = rel.begin(); it != rel.end() && *it != "."; ++it)
    {
        if (std::next(it) == rel.end()) break;
        p /= *it;
        if (estLien(p)) throw Error("suppression refusée : " + u8(p) + " est un lien");
    }
}

bool estEntree(const fs::path& p)
{
    std::error_code ec;
    if (!fs::is_regular_file(fs::symlink_status(p, ec))) return false;
    std::string x = u8(p.extension());
    for (char& ch : x) ch = char(std::tolower((unsigned char)ch));
    if (x == ".iso" || x == ".xiso") return true;
    std::ifstream f(p, std::ios::binary);
    char m[20];
    if (f.read(m, 4) && (!std::memcmp(m, "LIVE", 4) || !std::memcmp(m, "PIRS", 4) || !std::memcmp(m, "CON ", 4))) return true;   // STFS (jeu, TU)
    for (auto& l : Xiso::LAYOUTS)
        if (f.clear(), f.seekg(std::streamoff(l.offset + Xiso::VOLUME_SECTOR * Xiso::SECTOR)) && f.read(m, 20) && !std::memcmp(m, Xiso::MAGIC, 20)) return true;
    return false;
}

uint64_t supprimerDans(const fs::path& racine, const fs::path& cible, std::vector<fs::path>* gardes)
{
    verifierSuppression(racine, cible);
    const fs::path c = sansSeparateurFinal(cible);
    std::error_code ec;
    if (fs::symlink_status(c, ec).type() == fs::file_type::not_found) return 0;
    uint64_t n = 0;
    effacer(c, n, gardes);
    return n;
}

uint64_t tailleSansLiens(const fs::path& p)
{
    uint64_t t = 0;
    std::error_code ec;
    if (estLien(p)) return 0;
    auto s = fs::symlink_status(p, ec);
    if (fs::is_regular_file(s)) return fs::file_size(p, ec);
    if (!fs::is_directory(s)) return 0;
    for (auto it = fs::directory_iterator(p, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) t += tailleSansLiens(it->path());
    return t;
}

int64_t migrerPortable(const fs::path& ancien, const fs::path& data)
{
    const fs::path src = ancien / "userdata", fait = data / ".portable-copie";
    std::error_code ec;
    if (!fs::exists(ancien / ".wakfurecomp", ec) || estLien(src) || !fs::is_directory(src, ec) || fs::is_empty(src, ec)
        || fs::exists(fait, ec))
        return -1;
    const fs::path r = cle(fs::weakly_canonical(data)).lexically_relative(cle(fs::weakly_canonical(src)));
    if (!r.empty() && *r.begin() != "..") return -1;   // données dans l'ancien dossier (même dossier) : rien à copier
    int64_t n = 0;
    fs::create_directories(data);
    for (auto it = fs::recursive_directory_iterator(src); it != fs::recursive_directory_iterator(); ++it)
    {
        const fs::path d = data / it->path().lexically_relative(src);
        if (estLien(it->path())) it.disable_recursion_pending();   // lien ou jonction : ni suivi ni copié
        else if (it->is_directory()) fs::create_directories(d);
        else if (it->is_regular_file() && !fs::exists(fs::symlink_status(d)))
        {
            fs::copy_file(it->path(), d, fs::copy_options::none);   // none : erreur plutôt qu'écraser
            ++n;
        }
    }
    std::ofstream o(fait);   // témoin écrit seulement après une copie complète (sinon refaite au lancement suivant)
    if (!o.flush()) throw Error("écriture impossible : " + u8(fait));
    return n;
}

} // namespace wk
