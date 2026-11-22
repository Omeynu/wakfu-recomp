// Écriture PNG RGBA 8 bits sans dépendance : deflate en blocs « stockés » (non compressés).
#include "png.h"
#include <cstdio>

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

}  // namespace wk
