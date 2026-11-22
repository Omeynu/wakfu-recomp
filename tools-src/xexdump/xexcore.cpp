#include "xexcore.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <image.h>
#include <xex_patcher.h>

std::vector<uint8_t> readAllBytes(const std::filesystem::path& p, bool& ok)
{
    std::ifstream f(p, std::ios::binary);
    ok = bool(f);
    std::vector<uint8_t> d{ std::istreambuf_iterator<char>(f), {} };
    ok = ok && !f.bad();
    return d;
}

bool writeAllBytes(const std::filesystem::path& p, const uint8_t* d, size_t n)
{
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(d), n);
    f.close();
    return !f.fail();
}

bool xexPrepare(std::vector<uint8_t> xex, const std::vector<uint8_t>& patch,
                const std::filesystem::path& patchedXex, const std::filesystem::path& image,
                std::ostream& listing, std::string& err)
{
    if (!patch.empty())
    {
        std::vector<uint8_t> out;
        auto r = XexPatcher::apply(xex.data(), xex.size(), patch.data(), patch.size(), out, false);
        if (r != XexPatcher::Result::Success) { err = "patch refusé (code " + std::to_string(int(r)) + ")"; return false; }
        if (!writeAllBytes(patchedXex, out.data(), out.size())) { err = "écriture impossible : " + patchedXex.u8string(); return false; }
        xex.swap(out);
    }
    Image img = Image::ParseImage(xex.data(), xex.size());
    if (!img.data) { err = "image XEX illisible (chiffrement/compression ?)"; return false; }
    if (!writeAllBytes(image, img.data.get(), img.size)) { err = "écriture impossible : " + image.u8string(); return false; }
    char line[512];
    std::snprintf(line, sizeof line, "base 0x%08zX\nsize 0x%08X\nentry 0x%08zX\n", img.base, img.size, img.entry_point);
    listing << line;
    for (auto& s : img.sections)
    {
        std::snprintf(line, sizeof line, "section %-8.8s 0x%08zX 0x%08X %s\n", s.name.c_str(), s.base, s.size, (s.flags & SectionFlags_Code) ? "code" : "data");
        listing << line;
    }
    for (auto& s : img.symbols)
    {
        std::snprintf(line, sizeof line, "import 0x%08zX %s\n", s.address, s.name.c_str());
        listing << line;
    }
    listing.flush();
    if (!listing) { err = "écriture de la liste impossible"; return false; }
    return true;
}
