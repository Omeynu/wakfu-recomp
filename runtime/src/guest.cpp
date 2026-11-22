#include "guest.h"
#include <atomic>

namespace wk {

uint8_t* g_base = nullptr;

std::string ReadAnsi(uint32_t a, size_t max) {
    std::string s;
    if (!a) return s;
    for (size_t i = 0; i < max; i++) {
        char c = char(Ld8(a + i));
        if (!c) break;
        s.push_back(c);
    }
    return s;
}

std::string ReadWide(uint32_t a, size_t max) {
    std::string s;
    if (!a) return s;
    for (size_t i = 0; i < max; i++) {
        uint16_t c = Ld16(a + 2 * i);
        if (!c) break;
        s.push_back(c < 0x80 ? char(c) : '?');
    }
    return s;
}

namespace {
struct Region { uint32_t next, end; };
// Zones invitées : à l'écart de l'image (0x82000000-0x826C0000) et de la table des fonctions
// hôte (0x826C0000-0x83110000) que PPC_LOOKUP_FUNC place juste après l'image. Piles sous les registres XMA
// (0x7FEA0000, kernel/xma.h).
std::atomic<uint32_t> g_next[4] = {0x70000000u, 0x40000000u, 0xA0000000u, 0x7F000000u};
constexpr uint32_t g_end[4] = {0x7F000000u, 0x70000000u, 0xC0000000u, 0x7FEA0000u};
}  // namespace

uint32_t Alloc(Heap h, uint32_t size, uint32_t align) {
    const int i = int(h);
    if (align < 16) align = 16;
    uint32_t cur = g_next[i].load();
    for (;;) {
        uint64_t start = (uint64_t(cur) + align - 1) & ~uint64_t(align - 1);
        uint64_t end = start + ((uint64_t(size) + 15) & ~uint64_t(15));
        if (end > g_end[i]) return 0;
        if (g_next[i].compare_exchange_weak(cur, uint32_t(end))) {
            memset(g_base + start, 0, end - start);
            return uint32_t(start);
        }
    }
}

}  // namespace wk
