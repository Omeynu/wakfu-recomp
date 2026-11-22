// Portage C++ de runtime/tools/gen_imports.py (l'installateur Windows n'embarque pas Python).
// Sorties identiques octet pour octet (vérifié par packaging/windows/build-installer.sh avec un jeu préparé).
#include "gen_imports.h"
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {

std::string u8(const std::filesystem::path& p) { auto s = p.u8string(); return std::string(s.begin(), s.end()); }

std::string readAll(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("lecture impossible : " + u8(p));
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

struct Be
{
    const std::string& x;
    uint32_t u32(uint64_t o) const
    {
        if (o + 4 > x.size()) throw std::runtime_error("XEX tronqué");
        auto b = reinterpret_cast<const unsigned char*>(x.data()) + o;
        return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3];
    }
    uint16_t u16(uint64_t o) const
    {
        if (o + 2 > x.size()) throw std::runtime_error("XEX tronqué");
        auto b = reinterpret_cast<const unsigned char*>(x.data()) + o;
        return uint16_t(b[0] << 8 | b[1]);
    }
};

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...)
{
    char b[512];
    va_list a;
    va_start(a, f);
    std::vsnprintf(b, sizeof b, f, a);
    va_end(a);
    return b;
}

void writeFile(const std::filesystem::path& p, const std::string& s)
{
    std::ofstream f(p, std::ios::binary);   // fins de ligne LF, comme le script Python sous Linux
    f << s;
    if (!f) throw std::runtime_error("écriture impossible : " + u8(p));
}

} // namespace

std::string genImports(const std::filesystem::path& xexPath, const std::filesystem::path& dump,
                       const std::filesystem::path& tables, const std::filesystem::path& out)
{
    std::string x = readAll(xexPath);
    Be be{ x };
    if (x.compare(0, 4, "XEX2") != 0) throw std::runtime_error("pas un XEX2");
    uint32_t headerSize = be.u32(8);
    std::map<uint32_t, uint32_t> hdr;
    for (uint32_t i = 0, n = be.u32(0x14); i < n; ++i) hdr[be.u32(0x18 + i * 8)] = be.u32(0x18 + i * 8 + 4);
    if (!hdr.count(0x3FF) || !hdr.count(0x103FF)) throw std::runtime_error("en-têtes XEX manquants");
    uint16_t enc = be.u16(hdr[0x3FF] + 4), comp = be.u16(hdr[0x3FF] + 6);
    if (enc != 0 || comp > 1) throw std::runtime_error("XEX chiffré ou compressé : non géré");
    if (comp == 1) throw std::runtime_error("compression basique (blocs) : non gérée");
    const uint32_t base = 0x82000000;
    auto img = [&](uint32_t a) { return be.u32(uint64_t(headerSize) + a - base); };

    std::map<std::pair<std::string, uint32_t>, std::pair<std::string, std::string>> names;
    static const std::regex re(R"(XE_EXPORT\(\w+,\s*(0x[0-9A-Fa-f]+),\s*(\w+),\s*(\w+)\))");
    for (auto [lib, file] : { std::pair{ "xboxkrnl.exe", "xboxkrnl_table.inc" }, std::pair{ "xam.xex", "xam_table.inc" } })
    {
        std::string t = readAll(tables / file);
        for (std::sregex_iterator it(t.begin(), t.end(), re), end; it != end; ++it)
            names[{ lib, uint32_t(std::stoul((*it)[1].str(), nullptr, 16)) }] = { (*it)[2].str(), (*it)[3].str() };
    }

    uint32_t il = hdr[0x103FF], strSize = be.u32(il + 4), nlib = be.u32(il + 8);
    std::vector<std::string> libnames;
    for (uint64_t o = il + 12; libnames.size() < nlib;)
    {
        size_t z = x.find('\0', o);
        if (z == std::string::npos || o >= x.size()) throw std::runtime_error("noms de bibliothèques invalides");
        libnames.push_back(x.substr(o, z - o));
        o += (libnames.back().size() + 1 + 3) & ~size_t(3);
    }
    struct Var { uint32_t rec; std::string lib; uint32_t ord; std::string name; };
    std::vector<Var> vars;
    uint64_t o = uint64_t(il) + 12 + strSize;
    for (uint32_t l = 0; l < nlib; ++l)
    {
        uint32_t size = be.u32(o);
        uint16_t nameIdx = be.u16(o + 0x24), n = be.u16(o + 0x26);
        if (nameIdx >= libnames.size() || size == 0) throw std::runtime_error("table d'imports invalide");
        const std::string& lib = libnames[nameIdx];
        for (uint32_t j = 0; j < n; ++j)
        {
            uint32_t rec = be.u32(o + 0x28 + 4 * j), v = img(rec);
            if (v >> 24 == 0)
            {
                auto it = names.find({ lib, v & 0xFFFF });
                if (it != names.end() && it->second.second == "kVariable")
                    vars.push_back({ rec, lib, v & 0xFFFF, it->second.first });
            }
        }
        o += size;
    }

    std::vector<std::pair<uint32_t, std::string>> funcs;
    std::istringstream d(readAll(dump));
    for (std::string line; std::getline(d, line);)
    {
        if (line.rfind("import ", 0) != 0) continue;
        std::istringstream ls(line);
        std::string kw, addr, name;
        ls >> kw >> addr >> name;
        funcs.push_back({ uint32_t(std::stoul(addr, nullptr, 16)), name.size() > 7 ? name.substr(7) : "" });
    }

    std::filesystem::create_directories(out);
    std::string list = "// Généré par runtime/tools/gen_imports.py\n", kv = list,
                stub = "// Généré par runtime/tools/gen_imports.py : stubs faibles, remplacés par les implémentations.\n"
                       "#include \"kernel/stub.h\"\n\n";
    for (auto& [a, nm] : funcs)
    {
        list += fmt("X(0x%08X, %s)\n", a, nm.c_str());
        stub += fmt("__attribute__((weak)) PPC_FUNC(__imp__%s) { wk::StubCall(\"%s\", 0x%08X, ctx); }\n", nm.c_str(), nm.c_str(), a);
    }
    for (auto& v : vars) kv += fmt("V(0x%08X, \"%s\", %u, %s)\n", v.rec, v.lib.c_str(), v.ord, v.name.c_str());
    writeFile(out / "imports_list.inc", list);
    writeFile(out / "kernel_vars.inc", kv);
    writeFile(out / "imports_stub.cpp", stub);
    return fmt("%zu fonctions, %zu variables", funcs.size(), vars.size());
}
