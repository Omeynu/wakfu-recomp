// Écriture PNG RGBA 8 bits sans dépendance : deflate en blocs « stockés » (non compressés).
#include "png.h"
#include <cstdio>
#include <fstream>
#include <iterator>

namespace wk {

namespace {
uint32_t Crc(const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFFu) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            table[i] = v;
        }
        init = true;
    }
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}
void Be32(std::vector<uint8_t>& o, uint32_t v) { for (int s = 24; s >= 0; s -= 8) o.push_back(uint8_t(v >> s)); }
void Chunk(FILE* f, const char* type, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> buf;
    Be32(buf, uint32_t(data.size()));
    buf.insert(buf.end(), type, type + 4);
    buf.insert(buf.end(), data.begin(), data.end());
    Be32(buf, Crc(buf.data() + 4, buf.size() - 4) ^ 0xFFFFFFFFu);
    fwrite(buf.data(), 1, buf.size(), f);
}
}  // namespace

bool WritePng(const std::string& path, uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba) {
    if (rgba.size() != size_t(w) * h * 4) return false;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr;
    Be32(ihdr, w); Be32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});        // 8 bits, RGBA, deflate, filtre 0, sans entrelacement
    Chunk(f, "IHDR", ihdr);
    std::vector<uint8_t> raw;                         // lignes précédées du filtre 0
    raw.reserve(size_t(h) * (w * 4 + 1));
    for (uint32_t y = 0; y < h; y++) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba.begin() + size_t(y) * w * 4, rgba.begin() + size_t(y + 1) * w * 4);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    for (size_t i = 0; i < raw.size(); i += 65535) {
        const size_t n = raw.size() - i < 65535 ? raw.size() - i : 65535;
        z.push_back(i + n == raw.size() ? 1 : 0);
        z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n)); z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), raw.begin() + i, raw.begin() + i + n);
    }
    Be32(z, (b << 16) | a);
    Chunk(f, "IDAT", z);
    Chunk(f, "IEND", {});
    return fclose(f) == 0;
}


bool PngVersIco(const std::string& png, const std::string& ico) {
    std::ifstream in(png, std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), {});
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (d.size() < 24 || !std::equal(sig, sig + 8, d.begin()) || std::string(d.begin() + 12, d.begin() + 16) != "IHDR") return false;
    auto be = [&](size_t o) { return uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 | d[o + 3]; };
    const uint32_t w = be(16), h = be(20), n = uint32_t(d.size());
    if (w < 1 || w > 256 || h < 1 || h > 256) return false;
    const uint8_t t[22] = {0, 0, 1, 0, 1, 0,                                   // ICONDIR : type 1 (icône), 1 image
                           uint8_t(w), uint8_t(h), 0, 0, 1, 0, 32, 0,          // 256 s'écrit 0
                           uint8_t(n), uint8_t(n >> 8), uint8_t(n >> 16), uint8_t(n >> 24), 22, 0, 0, 0};
    std::ofstream out(ico, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(t), 22).write(reinterpret_cast<const char*>(d.data()), n);
    return bool(out.flush());
}


// Police 5x8 (lignes 0-6, ligne 7 : jambages), formes du jeu de caractères 5x8 courant des afficheurs LCD : ASCII
// imprimable sauf quelques signes, et les lettres accentuées du français. 8 lignes de 5 bits.
namespace {
struct Glyphe { uint32_t code; uint8_t l[8]; };
constexpr Glyphe kPolice[] = {
    {'0', {14, 17, 19, 21, 25, 17, 14}}, {'1', {4, 12, 4, 4, 4, 4, 14}},     {'2', {14, 17, 1, 2, 4, 8, 31}},
    {'3', {31, 2, 4, 2, 1, 17, 14}},     {'4', {2, 6, 10, 18, 31, 2, 2}},    {'5', {31, 16, 30, 1, 1, 17, 14}},
    {'6', {6, 8, 16, 30, 17, 17, 14}},   {'7', {31, 1, 2, 4, 8, 8, 8}},      {'8', {14, 17, 17, 14, 17, 17, 14}},
    {'9', {14, 17, 17, 15, 1, 2, 12}},
    {'A', {14, 17, 17, 17, 31, 17, 17}}, {'B', {30, 17, 17, 30, 17, 17, 30}}, {'C', {14, 17, 16, 16, 16, 17, 14}},
    {'D', {28, 18, 17, 17, 17, 18, 28}}, {'E', {31, 16, 16, 30, 16, 16, 31}}, {'F', {31, 16, 16, 30, 16, 16, 16}},
    {'G', {14, 17, 16, 23, 17, 17, 15}}, {'H', {17, 17, 17, 31, 17, 17, 17}}, {'I', {14, 4, 4, 4, 4, 4, 14}},
    {'J', {7, 2, 2, 2, 2, 18, 12}},      {'K', {17, 18, 20, 24, 20, 18, 17}}, {'L', {16, 16, 16, 16, 16, 16, 31}},
    {'M', {17, 27, 21, 21, 17, 17, 17}}, {'N', {17, 17, 25, 21, 19, 17, 17}}, {'O', {14, 17, 17, 17, 17, 17, 14}},
    {'P', {30, 17, 17, 30, 16, 16, 16}}, {'Q', {14, 17, 17, 17, 21, 18, 13}}, {'R', {30, 17, 17, 30, 20, 18, 17}},
    {'S', {15, 16, 16, 14, 1, 1, 30}},   {'T', {31, 4, 4, 4, 4, 4, 4}},      {'U', {17, 17, 17, 17, 17, 17, 14}},
    {'V', {17, 17, 17, 17, 17, 10, 4}},  {'W', {17, 17, 17, 21, 21, 21, 10}}, {'X', {17, 17, 10, 4, 10, 17, 17}},
    {'Y', {17, 17, 17, 10, 4, 4, 4}},    {'Z', {31, 1, 2, 4, 8, 16, 31}},
    {'a', {0, 0, 14, 1, 15, 17, 15}},    {'b', {16, 16, 22, 25, 17, 17, 30}}, {'c', {0, 0, 14, 16, 16, 17, 14}},
    {'d', {1, 1, 13, 19, 17, 17, 15}},   {'e', {0, 0, 14, 17, 31, 16, 14}},  {'f', {6, 9, 8, 28, 8, 8, 8}},
    {'g', {0, 0, 15, 17, 17, 15, 1, 14}}, {'h', {16, 16, 22, 25, 17, 17, 17}}, {'i', {4, 0, 12, 4, 4, 4, 14}},
    {'j', {2, 0, 6, 2, 2, 2, 18, 12}},   {'k', {16, 16, 18, 20, 24, 20, 18}}, {'l', {12, 4, 4, 4, 4, 4, 14}},
    {'m', {0, 0, 26, 21, 21, 17, 17}},   {'n', {0, 0, 22, 25, 17, 17, 17}},  {'o', {0, 0, 14, 17, 17, 17, 14}},
    {'p', {0, 0, 30, 17, 17, 30, 16, 16}}, {'q', {0, 0, 15, 17, 17, 15, 1, 1}}, {'r', {0, 0, 22, 25, 16, 16, 16}},
    {'s', {0, 0, 15, 16, 14, 1, 30}},    {'t', {8, 8, 28, 8, 8, 9, 6}},      {'u', {0, 0, 17, 17, 17, 19, 13}},
    {'v', {0, 0, 17, 17, 17, 10, 4}},    {'w', {0, 0, 17, 17, 21, 21, 10}},  {'x', {0, 0, 17, 10, 4, 10, 17}},
    {'y', {0, 0, 17, 17, 17, 15, 1, 14}}, {'z', {0, 0, 31, 2, 4, 8, 31}},
    {'.', {0, 0, 0, 0, 0, 12, 12}},      {',', {0, 0, 0, 0, 12, 4, 8}},      {':', {0, 12, 12, 0, 12, 12, 0}},
    {';', {0, 12, 12, 0, 12, 4, 8}},     {'!', {4, 4, 4, 4, 4, 0, 4}},       {'?', {14, 17, 1, 2, 4, 0, 4}},
    {'-', {0, 0, 0, 31, 0, 0, 0}},       {'\'', {12, 4, 8, 0, 0, 0, 0}},     {'"', {10, 10, 10, 0, 0, 0, 0}},
    {'/', {0, 1, 2, 4, 8, 16, 0}},       {'(', {2, 4, 8, 8, 8, 4, 2}},       {')', {8, 4, 2, 2, 2, 4, 8}},
    {'%', {24, 25, 2, 4, 8, 19, 3}},     {'&', {12, 18, 20, 8, 21, 18, 13}}, {'+', {0, 4, 4, 31, 4, 4, 0}},
    {'_', {0, 0, 0, 0, 0, 0, 31}},
    {0xE0, {8, 4, 14, 1, 15, 17, 15}},   {0xE2, {4, 10, 14, 1, 15, 17, 15}}, {0xE7, {0, 0, 14, 16, 16, 17, 14, 4}},
    {0xE8, {8, 4, 14, 17, 31, 16, 14}},  {0xE9, {2, 4, 14, 17, 31, 16, 14}}, {0xEA, {4, 10, 14, 17, 31, 16, 14}},
    {0xEB, {10, 0, 14, 17, 31, 16, 14}}, {0xEE, {4, 10, 0, 12, 4, 4, 14}},   {0xEF, {10, 0, 12, 4, 4, 4, 14}},
    {0xF4, {4, 10, 0, 14, 17, 17, 14}},  {0xF9, {8, 4, 0, 17, 17, 19, 13}},  {0xFB, {4, 10, 0, 17, 17, 19, 13}},
    {0xC9, {2, 4, 31, 16, 30, 16, 31}},
};
}

std::vector<uint8_t> TexteRgba(const std::string& utf8, uint32_t echelle, uint32_t* w, uint32_t* h) {
    std::vector<uint32_t> codes;   // UTF-8 sur 1 ou 2 octets (latin) ; le reste devient une espace par caractère
    for (size_t i = 0; i < utf8.size(); i++) {
        const uint8_t c = uint8_t(utf8[i]);
        if (c < 0x80) codes.push_back(c);
        else if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size()) codes.push_back((c & 0x1Fu) << 6 | (uint8_t(utf8[++i]) & 0x3Fu));
        else if ((c & 0xC0) != 0x80) codes.push_back(' ');   // 3 ou 4 octets : une espace (suite ignorée)
    }
    const uint32_t e = echelle ? echelle : 1, marge = 4 * e;
    *w = uint32_t(codes.size()) * 6 * e - e + 2 * marge;
    *h = 8 * e + 2 * marge;
    std::vector<uint8_t> rgba(size_t(*w) * *h * 4);
    for (size_t i = 0; i < rgba.size(); i += 4) rgba[i] = rgba[i + 1] = rgba[i + 2] = 24, rgba[i + 3] = 255;
    for (size_t k = 0; k < codes.size(); k++) {
        const Glyphe* g = nullptr;
        for (const Glyphe& p : kPolice) if (p.code == codes[k]) g = &p;
        if (!g) continue;
        for (uint32_t y = 0; y < 8 * e; y++)
            for (uint32_t x = 0; x < 5 * e; x++)
                if (g->l[y / e] >> (4 - x / e) & 1) {
                    uint8_t* p = &rgba[((size_t(marge) + y) * *w + marge + k * 6 * e + x) * 4];
                    p[0] = p[1] = p[2] = 255;
                }
    }
    return rgba;
}
}  // namespace wk
