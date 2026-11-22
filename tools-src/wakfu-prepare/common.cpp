#include "common.h"
#include <algorithm>
#include <cctype>
#include <vector>

std::string u8(const fs::path& p) { return p.u8string(); }

std::string lower(std::string s)
{
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

InputFile::InputFile(const fs::path& p) : path_(p), f_(p, std::ios::binary)
{
    if (!f_) throw PrepareError("ouverture impossible : " + u8(p));
    std::error_code ec;
    size_ = fs::file_size(p, ec);
    if (ec) throw PrepareError("taille illisible : " + u8(p));
}

bool InputFile::tryRead(uint64_t off, void* dst, size_t n)
{
    if (off > size_ || n > size_ - off) return false;
    f_.clear();
    f_.seekg(std::streamoff(off));
    f_.read(static_cast<char*>(dst), std::streamsize(n));
    return size_t(f_.gcount()) == n;
}

void InputFile::read(uint64_t off, void* dst, size_t n)
{
    if (!tryRead(off, dst, n))
        throw PrepareError(u8(path_) + " : lecture impossible à l'offset " + std::to_string(off) + " (fichier tronqué ?)");
}

void checkName(const std::string& name)
{
    auto bad = [&](const char* why) { throw PrepareError("nom de fichier refusé (" + std::string(why) + ") : « " + name + " »"); };
    if (name.empty() || name == "." || name == "..") bad("vide ou relatif");
    if (name.size() > 255) bad("trop long");
    for (unsigned char c : name)
        if (c < 0x20 || c >= 0x7F || std::string_view("<>:\"/\\|?*").find(char(c)) != std::string_view::npos) bad("caractère interdit");
    if (name.back() == '.' || name.back() == ' ') bad("point ou espace final");
    std::string stem = lower(name.substr(0, name.find('.')));
    static const char* reserved[] = { "con", "prn", "aux", "nul" };
    for (auto r : reserved) if (stem == r) bad("nom réservé");
    if (stem.size() == 4 && (stem.compare(0, 3, "com") == 0 || stem.compare(0, 3, "lpt") == 0) && stem[3] >= '0' && stem[3] <= '9') bad("nom réservé");
}

fs::path NewPaths::child(const fs::path& dir, const std::string& name)
{
    checkName(name);
    fs::path p = dir / fs::u8path(name);
    std::error_code ec;
    if (!seen_.insert(lower(u8(p))).second || fs::exists(fs::symlink_status(p, ec)))
        throw PrepareError("deux entrées portent le même nom (casse ignorée) : " + u8(p));
    return p;
}

void copyRange(InputFile& src, uint64_t off, uint64_t n, const fs::path& dst)
{
    std::ofstream o(dst, std::ios::binary);
    if (!o) throw PrepareError("création impossible : " + u8(dst));
    std::vector<char> buf(1 << 20);
    while (n)
    {
        size_t k = size_t(std::min<uint64_t>(n, buf.size()));
        src.read(off, buf.data(), k);
        o.write(buf.data(), std::streamsize(k));
        off += k;
        n -= k;
    }
    o.close();
    if (o.fail()) throw PrepareError("écriture impossible : " + u8(dst));
}
