// Tests de l'extraction :
//   test_extract unit <dossier_tmp>                 adressage STFS, noms, XISO synthétiques (offsets XISO/XGD2/XGD3), images piégées
//   test_extract stfs <dossier_paquets> <dossier_tmp>  paquet TU réel : extraction + détection d'un bloc corrompu
//   test_extract make-xiso <dossier> <image> <offset>  écrit une image XDVDFS à partir d'un dossier
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <vector>
#include "stfs.h"
#include "xiso.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "ÉCHEC %s:%d : %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

template <class F> static bool throws(F f) { try { f(); } catch (const PrepareError&) { return true; } return false; }

// ---------- écriture XDVDFS (pour les tests) ----------
// Arbre binaire équilibré sur les noms triés (casse ignorée), entrées alignées sur 4 octets,
// jamais à cheval sur deux secteurs. Les secteurs sont relatifs au début de la partition.
struct XisoWriter
{
    std::ofstream o;
    uint64_t base;
    uint32_t next = Xiso::VOLUME_SECTOR + 1;

    struct Node { std::string name; bool dir; uint32_t sector, size; };

    void writeAt(uint64_t sector, const void* d, size_t n)
    {
        o.seekp(std::streamoff(base + sector * Xiso::SECTOR));
        o.write(static_cast<const char*>(d), std::streamsize(n));
    }
    static void put16(std::vector<uint8_t>& t, size_t at, uint32_t v) { t[at] = uint8_t(v); t[at + 1] = uint8_t(v >> 8); }
    static void put32(std::vector<uint8_t>& t, size_t at, uint32_t v) { for (int i = 0; i < 4; ++i) t[at + i] = uint8_t(v >> (8 * i)); }

    // Table brute d'un dossier à partir de ses nœuds (forme d'arbre choisie par l'appelant si besoin).
    static std::vector<uint8_t> table(std::vector<Node> nodes)
    {
        std::sort(nodes.begin(), nodes.end(), [](auto& a, auto& b) { return lower(a.name) < lower(b.name); });
        std::vector<uint32_t> pos(nodes.size());
        uint32_t end = 0;
        std::function<void(int, int)> place = [&](int lo, int hi) {
            if (lo >= hi) return;
            int mid = (lo + hi) / 2;
            uint32_t len = (14 + uint32_t(nodes[mid].name.size()) + 3) & ~3u;
            if (end % Xiso::SECTOR + len > Xiso::SECTOR) end = (end / Xiso::SECTOR + 1) * Xiso::SECTOR;
            pos[mid] = end;
            end += len;
            place(lo, mid);
            place(mid + 1, hi);
        };
        place(0, int(nodes.size()));
        std::vector<uint8_t> t((end + Xiso::SECTOR - 1) / Xiso::SECTOR * Xiso::SECTOR, 0xFF);
        std::function<void(int, int)> emit = [&](int lo, int hi) {
            if (lo >= hi) return;
            int mid = (lo + hi) / 2, l = (lo + mid) / 2, r = (mid + 1 + hi) / 2;
            size_t at = pos[mid];
            put16(t, at, lo < mid ? pos[l] / 4 : 0);
            put16(t, at + 2, mid + 1 < hi ? pos[r] / 4 : 0);
            put32(t, at + 4, nodes[mid].sector);
            put32(t, at + 8, nodes[mid].size);
            t[at + 12] = nodes[mid].dir ? 0x10 : 0x80;
            t[at + 13] = uint8_t(nodes[mid].name.size());
            memcpy(&t[at + 14], nodes[mid].name.data(), nodes[mid].name.size());
            emit(lo, mid);
            emit(mid + 1, hi);
        };
        emit(0, int(nodes.size()));
        return t;
    }

    std::pair<uint32_t, uint32_t> putTable(const std::vector<uint8_t>& t)
    {
        if (t.empty()) return { 0, 0 };
        uint32_t s = next;
        next += uint32_t(t.size() / Xiso::SECTOR);
        writeAt(s, t.data(), t.size());
        return { s, uint32_t(t.size()) };
    }

    std::pair<uint32_t, uint32_t> dir(const fs::path& d)
    {
        std::vector<Node> nodes;
        for (auto& e : fs::directory_iterator(d))
        {
            Node n{ u8(e.path().filename()), e.is_directory(), 0, 0 };
            if (n.dir) std::tie(n.sector, n.size) = dir(e.path());
            else
            {
                InputFile f(e.path());
                n.size = uint32_t(f.size());
                n.sector = n.size ? next : 0;
                next += uint32_t((f.size() + Xiso::SECTOR - 1) / Xiso::SECTOR);
                std::vector<char> buf(1 << 20);
                for (uint64_t off = 0; off < f.size();)
                {
                    size_t k = size_t(std::min<uint64_t>(buf.size(), f.size() - off));
                    f.read(off, buf.data(), k);
                    o.seekp(std::streamoff(base + uint64_t(n.sector) * Xiso::SECTOR + off));
                    o.write(buf.data(), std::streamsize(k));
                    off += k;
                }
            }
            nodes.push_back(n);
        }
        return putTable(table(nodes));
    }

    void volume(uint32_t rootSector, uint32_t rootSize)
    {
        std::vector<uint8_t> vd(Xiso::SECTOR, 0);
        memcpy(vd.data(), Xiso::MAGIC, 20);
        put32(vd, 20, rootSector);
        put32(vd, 24, rootSize);
        memcpy(vd.data() + 0x7EC, Xiso::MAGIC, 20);
        writeAt(Xiso::VOLUME_SECTOR, vd.data(), vd.size());
        // Taille finale alignée sur un secteur (les données peuvent finir en milieu de secteur).
        std::vector<uint8_t> z(Xiso::SECTOR, 0);
        writeAt(next, z.data(), z.size());
    }
};

static void makeXiso(const fs::path& src, const fs::path& iso, uint64_t offset)
{
    XisoWriter w{ std::ofstream(iso, std::ios::binary | std::ios::trunc), offset };
    auto [s, n] = w.dir(src);
    w.volume(s, n);
    w.o.close();
    if (w.o.fail()) throw PrepareError("écriture de l'image impossible");
}

// Image piégée : une racine dont la seule entrée a le nom donné / pointe vers la racine elle-même.
static void makeRawXiso(const fs::path& iso, const std::vector<XisoWriter::Node>& root, bool rootLoop = false)
{
    XisoWriter w{ std::ofstream(iso, std::ios::binary | std::ios::trunc), 0 };
    auto nodes = root;
    if (rootLoop) for (auto& n : nodes) n.sector = w.next;   // le sous-dossier est la racine
    auto [s, n] = w.putTable(XisoWriter::table(nodes));
    w.volume(s, n);
}

static void writeFile(const fs::path& p, size_t n, uint8_t seed)
{
    std::ofstream o(p, std::ios::binary);
    for (size_t i = 0; i < n; ++i) o.put(char(uint8_t(i * 31 + seed + (i >> 11))));
}

static std::map<std::string, std::string> snapshot(const fs::path& root)
{
    std::map<std::string, std::string> m;
    for (auto& e : fs::recursive_directory_iterator(root))
    {
        std::string rel = e.path().lexically_relative(root).generic_u8string();
        if (e.is_directory()) m[rel] = "<dir>";
        else { std::ifstream f(e.path(), std::ios::binary); m[rel] = { std::istreambuf_iterator<char>(f), {} }; }
    }
    return m;
}

static void testAddressing()
{
    // Disposition (une table par niveau) : L0#0, données 0..169, L1, L0#1, données 170...
    CHECK(Stfs::dataBacking(0) == 1);
    CHECK(Stfs::dataBacking(169) == 170);
    CHECK(Stfs::l0Backing(170) == 0xAC);
    CHECK(Stfs::dataBacking(170) == 0xAD);
    std::vector<uint64_t> used;
    for (uint32_t b = 0; b < 3 * Stfs::L1; b += 37)
    {
        uint64_t d = Stfs::dataBacking(b);
        CHECK(d != Stfs::l0Backing(b));
        used.push_back(d);
    }
    std::sort(used.begin(), used.end());
    CHECK(std::adjacent_find(used.begin(), used.end()) == used.end());
}

static void testNames()
{
    for (const char* ok : { "default.xex", "Media", "a.b.c", "LPT", "com10" })
        CHECK(!throws([&] { checkName(ok); }));
    for (const char* bad : { "", ".", "..", "a/b", "a\\b", "c:x", "nul", "CON.txt", "com1", "x.", "x ", "a\tb", "a*b" })
        CHECK(throws([&] { checkName(bad); }));
}

static void testXiso(const fs::path& tmp)
{
    fs::path src = tmp / "src";
    fs::create_directories(src / "Media" / "Sous dossier");
    fs::create_directories(src / "vide");
    writeFile(src / "default.xex", 70000, 1);
    writeFile(src / "zero.bin", 0, 2);
    writeFile(src / "un.bin", 1, 3);
    writeFile(src / "Media" / "secteur.bin", Xiso::SECTOR, 4);
    writeFile(src / "Media" / "Sous dossier" / "c.dat", 5000, 5);
    // Dossier dont la table couvre plusieurs secteurs (noms longs)
    for (int i = 0; i < 150; ++i)
        writeFile(src / "Media" / ("fichier_au_nom_assez_long_" + std::to_string(i) + ".bin"), size_t(i * 13), uint8_t(i));
    auto want = snapshot(src);

    for (auto& l : Xiso::LAYOUTS)
    {
        if (l.offset == 0x18300000) continue;   // XGD1 : même code, image inutilement grosse
        fs::path iso = tmp / (std::string(l.name) + ".iso"), out = tmp / (std::string("out-") + l.name);
        makeXiso(src, iso, l.offset);
        {
            Xiso x(iso);
            CHECK(std::string(x.layout().name) == l.name);
            CHECK(x.hasRootFile("DEFAULT.XEX"));
            fs::create_directories(out);
            CHECK(x.extract(out) == 155);
            CHECK(snapshot(out) == want);
        }   // fermer l'image avant de la supprimer (Windows)
        fs::remove(iso);
    }

    // Images piégées : nom remontant d'un dossier, séparateur, boucle de dossiers, fichier hors de l'image
    auto rejected = [&](std::vector<XisoWriter::Node> root, bool loop) {
        fs::path iso = tmp / "piege.iso", out = tmp / "out-piege";
        fs::remove_all(out);
        fs::create_directories(out);
        makeRawXiso(iso, root, loop);
        return throws([&] { Xiso(iso).extract(out); }) && !fs::exists(tmp / "evil");
    };
    CHECK(rejected({ { "..", true, 0, 0 } }, false));
    CHECK(rejected({ { "../evil", false, 0, 0 } }, false));
    CHECK(rejected({ { "a\\..\\..\\evil", false, 0, 0 } }, false));
    CHECK(rejected({ { "boucle", true, 0, Xiso::SECTOR } }, true));
    CHECK(rejected({ { "gros.bin", false, 40, 1u << 30 } }, false));
    CHECK(rejected({ { "A.bin", false, 0, 0 }, { "a.bin", false, 0, 0 } }, false));
    CHECK(rejected({ { "nul", false, 0, 0 } }, false));
    // Pas de descripteur
    writeFile(tmp / "rien.iso", 0x20000, 9);
    InputFile f(tmp / "rien.iso");
    CHECK(Xiso::probe(f) == nullptr);
}

static void testStfs(const fs::path& pkgDir, const fs::path& tmp)
{
    fs::path tu;
    for (auto& e : fs::directory_iterator(pkgDir))
    {
        InputFile f(e.path());
        if (Stfs::probe(f) && Stfs(e.path()).hasRootFile("default.xexp")) tu = e.path();
    }
    CHECK(!tu.empty());
    if (tu.empty()) return;
    fs::path out = tmp / "tu";
    fs::create_directories(out);
    Stfs s(tu);
    CHECK(s.extract(out) > 0);
    std::ifstream x(out / "default.xexp", std::ios::binary);
    char m[4] = {};
    x.read(m, 4);
    CHECK(!memcmp(m, "XEX2", 4));
    // Un octet modifié dans la table des fichiers doit être détecté (SHA-1 du bloc)
    fs::path bad = tmp / "tu-corrompu";
    fs::copy_file(tu, bad, fs::copy_options::overwrite_existing);
    {
        std::fstream f(bad, std::ios::binary | std::ios::in | std::ios::out);
        uint64_t at = s.base() + Stfs::dataBacking(s.fileTableBlock()) * Stfs::BLOCK;
        f.seekg(std::streamoff(at));
        char c = char(f.get() ^ 0xFF);
        f.seekp(std::streamoff(at));
        f.put(c);
    }
    CHECK(throws([&] { Stfs t(bad); }));
}

int main(int argc, char** argv)
{
    try
    {
        std::string mode = argc > 1 ? argv[1] : "";
        if (mode == "make-xiso" && argc == 5) { makeXiso(argv[2], argv[3], std::stoull(argv[4], nullptr, 0)); return 0; }
        if (mode == "unit" && argc == 3)
        {
            fs::path tmp = argv[2];
            fs::remove_all(tmp);
            fs::create_directories(tmp);
            testAddressing();
            testNames();
            testXiso(tmp);
        }
        else if (mode == "stfs" && argc == 4)
        {
            fs::path tmp = argv[3];
            fs::remove_all(tmp);
            fs::create_directories(tmp);
            testStfs(argv[2], tmp);
        }
        else { std::fprintf(stderr, "usage : test_extract unit <tmp> | stfs <paquets> <tmp> | make-xiso <dossier> <image> <offset>\n"); return 2; }
    }
    catch (const std::exception& e) { std::fprintf(stderr, "exception : %s\n", e.what()); return 1; }
    if (failures) { std::fprintf(stderr, "%d échec(s)\n", failures); return 1; }
    std::printf("OK\n");
    return 0;
}
