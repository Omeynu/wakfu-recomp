// wakfu-prepare : prépare les fichiers du jeu de l'utilisateur pour la recompilation.
// Entrées acceptées (détection par contenu, pas par extension) :
//   - paquet STFS (LIVE/PIRS/CON) du jeu XBLA ou de la mise à jour (TU) ;
//   - image disque ISO/XISO (XDVDFS, partition à 0 ou offsets XGD1/XGD2/XGD3) ;
//   - dossier extrait (contenant default.xex, ou default.xexp pour la TU) ;
//   - dossier quelconque : les paquets STFS et images trouvés dedans (4 niveaux) sont examinés.
// Sortie : <sortie>/base (jeu), <sortie>/tu2 (TU), <sortie>/patched/{default.xex,image.bin,patched.txt}.
// Code de sortie : 0 prêt ; 1 erreur ; 2 usage ; 3 prêt, mais SHA-1 de l'image différent de celui attendu
// (version du jeu ou de la mise à jour non reconnue : sortie complète, la recompilation risque d'échouer).
#include <cstdio>
#include <iostream>
#include <memory>
#include <TinySHA1.hpp>
#include "stfs.h"
#include "xiso.h"
#include "xexcore.h"
#ifdef _WIN32
#include <windows.h>
#endif

namespace {

// Image recompilée par config/wakfu.toml (XEX de base + TU 2).
const char* EXPECTED_IMAGE_SHA1 = "332fe8150f156734fc68554138122d5081cb96d9";
constexpr int kVersionNonReconnue = 3;   // code de sortie (voir en tête du fichier)

enum class Kind { Stfs, Xiso, Dir };
enum class Role { Base, Tu };
struct Source { Kind kind; Role role; fs::path path; std::string desc; };

const char* kindName(Kind k) { return k == Kind::Stfs ? "paquet STFS" : k == Kind::Xiso ? "image disque" : "dossier"; }

fs::path findRootFile(const fs::path& dir, const std::string& name)
{
    for (auto& e : fs::directory_iterator(dir))
        if (e.is_regular_file() && lower(u8(e.path().filename())) == name) return e.path();
    return {};
}

// Classe un fichier ; false s'il n'est ni STFS ni XDVDFS. Les paquets sans default.xex(p) sont signalés et ignorés.
bool classifyFile(const fs::path& p, std::vector<Source>& out)
{
    InputFile f(p);
    if (Stfs::probe(f))
    {
        Stfs s(p);
        char tid[16];
        std::snprintf(tid, sizeof tid, "%08X", s.titleId());
        if (s.hasRootFile("default.xex")) out.push_back({ Kind::Stfs, Role::Base, p, std::string("jeu, titre ") + tid });
        else if (s.hasRootFile("default.xexp")) out.push_back({ Kind::Stfs, Role::Tu, p, std::string("mise à jour, titre ") + tid });
        else std::printf("ignoré (ni jeu ni mise à jour) : %s\n", u8(p).c_str());
        return true;
    }
    if (Xiso::probe(f))
    {
        Xiso x(p);
        if (x.hasRootFile("default.xex")) out.push_back({ Kind::Xiso, Role::Base, p, x.layout().name });
        else std::printf("ignoré (image sans default.xex) : %s\n", u8(p).c_str());
        return true;
    }
    return false;
}

void classify(const fs::path& in, std::vector<Source>& out)
{
    std::error_code ec;
    auto st = fs::status(in, ec);
    if (ec || !fs::exists(st)) throw PrepareError("entrée introuvable : " + u8(in));
    if (fs::is_directory(st))
    {
        bool xex = !findRootFile(in, "default.xex").empty(), xexp = !findRootFile(in, "default.xexp").empty();
        if (xex && xexp) throw PrepareError(u8(in) + " : contient default.xex et default.xexp, séparer le jeu et la mise à jour");
        if (xex) { out.push_back({ Kind::Dir, Role::Base, in, "jeu extrait" }); return; }
        if (xexp) { out.push_back({ Kind::Dir, Role::Tu, in, "mise à jour extraite" }); return; }
        for (auto it = fs::recursive_directory_iterator(in, fs::directory_options::skip_permission_denied); it != fs::recursive_directory_iterator(); ++it)
        {
            if (it.depth() >= 4) it.disable_recursion_pending();
            if (it->is_regular_file() && it->file_size() >= 0x1000) classifyFile(it->path(), out);
        }
        return;
    }
    if (!classifyFile(in, out)) throw PrepareError(u8(in) + " : format non reconnu (ni STFS, ni ISO/XISO, ni dossier)");
}

void copyTree(const fs::path& src, const fs::path& dst, NewPaths& np, int depth = 0)
{
    if (depth > 64) throw PrepareError("dossier trop profond : " + u8(src));
    for (auto& e : fs::directory_iterator(src))
    {
        fs::path p = np.child(dst, u8(e.path().filename()));
        if (e.is_symlink()) throw PrepareError("lien symbolique refusé : " + u8(e.path()));
        if (e.is_directory()) { fs::create_directory(p); copyTree(e.path(), p, np, depth + 1); }
        else if (e.is_regular_file()) fs::copy_file(e.path(), p);
        else throw PrepareError("fichier spécial refusé : " + u8(e.path()));
    }
}

size_t extract(const Source& s, const fs::path& dst)
{
    fs::create_directory(dst);
    switch (s.kind)
    {
    case Kind::Stfs: return Stfs(s.path).extract(dst);
    case Kind::Xiso: return Xiso(s.path).extract(dst);
    case Kind::Dir: { NewPaths np; copyTree(s.path, dst, np); return 0; }
    }
    return 0;
}

std::string sha1File(const fs::path& p)
{
    InputFile f(p);
    sha1::SHA1 s;
    std::vector<uint8_t> buf(1 << 20);
    for (uint64_t off = 0; off < f.size();)
    {
        size_t n = size_t(std::min<uint64_t>(buf.size(), f.size() - off));
        f.read(off, buf.data(), n);
        s.processBytes(buf.data(), n);
        off += n;
    }
    uint8_t d[20];
    s.finalize(d);
    std::string hex;
    char b[3];
    for (auto c : d) { std::snprintf(b, sizeof b, "%02x", c); hex += b; }
    return hex;
}

bool isInside(const fs::path& p, const fs::path& dir)
{
    auto a = fs::weakly_canonical(p), b = fs::weakly_canonical(dir);
    auto r = a.lexically_relative(b);
    return !r.empty() && *r.begin() != "..";
}

const Source& pick(const std::vector<Source>& all, Role role, const char* what)
{
    const Source* found = nullptr;
    for (auto& s : all)
        if (s.role == role)
        {
            if (found) throw PrepareError(std::string("plusieurs candidats pour ") + what + " : " + u8(found->path) + " et " + u8(s.path));
            found = &s;
        }
    if (!found) throw PrepareError(std::string("introuvable parmi les entrées : ") + what);
    return *found;
}

int run(const std::vector<fs::path>& inputs, const fs::path& outDir)
{
    std::vector<Source> sources;
    for (auto& in : inputs) classify(in, sources);
    const Source& base = pick(sources, Role::Base, "le jeu (default.xex)");
    const Source& tu = pick(sources, Role::Tu, "la mise à jour (default.xexp, TU 2 attendue)");
    for (auto* s : { &base, &tu })
    {
        std::printf("%s : %s (%s), %s\n", s == &base ? "jeu" : "mise à jour", kindName(s->kind), s->desc.c_str(), u8(s->path).c_str());
        if (s->kind == Kind::Dir && (isInside(outDir, s->path) || isInside(s->path, outDir)))
            throw PrepareError("le dossier de sortie ne doit pas contenir ni être contenu dans " + u8(s->path));
    }

    fs::create_directories(outDir);
    for (auto name : { "base", "tu2", "patched" })
        if (fs::exists(outDir / name))
            throw PrepareError(u8(outDir / name) + " existe déjà : le supprimer ou choisir un autre dossier de sortie");
    // Travail dans un dossier temporaire marqué, déplacé à la fin : pas de sortie à moitié écrite.
    fs::path tmp = outDir / ".wakfu-prepare.tmp", marker = tmp / ".wakfu-prepare";
    if (fs::exists(tmp))
    {
        if (!fs::exists(marker)) throw PrepareError(u8(tmp) + " existe et n'a pas été créé par wakfu-prepare");
        fs::remove_all(tmp);
    }
    fs::create_directory(tmp);
    std::ofstream(marker).close();
    bool reconnu = false;
    try
    {
        size_t n = extract(base, tmp / "base");
        std::printf("jeu extrait%s\n", base.kind == Kind::Dir ? " (copié)" : (" : " + std::to_string(n) + " fichiers").c_str());
        n = extract(tu, tmp / "tu2");
        std::printf("mise à jour extraite%s\n", tu.kind == Kind::Dir ? " (copiée)" : (" : " + std::to_string(n) + " fichiers").c_str());

        fs::path xexPath = findRootFile(tmp / "base", "default.xex"), xexpPath = findRootFile(tmp / "tu2", "default.xexp");
        if (xexPath.empty() || xexpPath.empty()) throw PrepareError("default.xex ou default.xexp absent après extraction");
        bool ok1, ok2;
        auto xex = readAllBytes(xexPath, ok1);
        auto xexp = readAllBytes(xexpPath, ok2);
        if (!ok1 || !ok2 || xex.empty() || xexp.empty()) throw PrepareError("lecture de default.xex / default.xexp impossible");
        fs::path patched = tmp / "patched";
        fs::create_directory(patched);
        std::ofstream listing(patched / "patched.txt", std::ios::binary);   // fins de ligne LF aussi sous Windows
        std::string err;
        if (!xexPrepare(std::move(xex), xexp, patched / "default.xex", patched / "image.bin", listing, err))
            throw PrepareError("application de la mise à jour : " + err);
        listing.close();
        std::string sha = sha1File(patched / "image.bin");
        std::printf("TU appliquée, image : SHA-1 %s\n", sha.c_str());
        reconnu = sha == EXPECTED_IMAGE_SHA1;
        if (!reconnu)
            std::printf("ATTENTION : version du jeu ou de la mise à jour non reconnue (SHA-1 attendu %s) ; "
                        "la recompilation risque d'échouer.\n", EXPECTED_IMAGE_SHA1);
        for (auto name : { "base", "tu2", "patched" }) fs::rename(tmp / name, outDir / name);
        fs::remove_all(tmp);
    }
    catch (...)
    {
        std::error_code ec;
        fs::remove_all(tmp, ec);
        throw;
    }
    std::printf("prêt : %s\n", u8(outDir).c_str());
    return reconnu ? 0 : kVersionNonReconnue;
}

int usage()
{
    std::fprintf(stderr,
        "usage : wakfu-prepare -o <dossier_sortie> <entrée> [<entrée>...]\n"
        "  entrée : paquet STFS (jeu ou mise à jour), image ISO/XISO, dossier extrait,\n"
        "           ou dossier contenant ces fichiers. Le jeu et la mise à jour (TU) sont tous deux requis.\n"
        "  sortie : base/, tu2/, patched/{default.xex,image.bin,patched.txt}\n"
        "  code de sortie : 0 prêt, 1 erreur, 2 usage, 3 prêt mais version du jeu ou de la mise à jour\n"
        "                   non reconnue (SHA-1 de l'image différent de celui attendu)\n");
    return 2;
}

int realMain(const std::vector<fs::path>& args)
{
    fs::path out;
    std::vector<fs::path> inputs;
    for (size_t i = 0; i < args.size(); ++i)
    {
        if (args[i] == "-o" && i + 1 < args.size()) out = args[++i];
        else if (args[i] == "-h" || args[i] == "--help" || args[i] == "-o") return usage();
        else inputs.push_back(args[i]);
    }
    if (out.empty() || inputs.empty()) return usage();
    try { return run(inputs, out); }
    catch (const std::exception& e)
    {
        std::fflush(stdout);
        std::fprintf(stderr, "erreur : %s\n", e.what());
        return 1;
    }
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    std::vector<fs::path> args(argv + 1, argv + argc);
    return realMain(args);
}
#else
int main(int argc, char** argv)
{
    std::vector<fs::path> args(argv + 1, argv + argc);
    return realMain(args);
}
#endif
