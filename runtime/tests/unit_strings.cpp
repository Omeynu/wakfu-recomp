// Test du formateur printf invité (chaînes en mémoire invitée, arguments 64 bits).
#include <cstdio>
#include <cstring>
#include <vector>
#include "../src/guest.h"
#include "../src/kernel/strings.h"
#include "../src/platform.h"
using namespace wk;
static int g_fail = 0;
struct Vec : ArgSource { std::vector<uint64_t> v; size_t i = 0; uint64_t Next() override { return i < v.size() ? v[i++] : 0; } };
static std::u16string U(const char* s) { std::u16string r; while (*s) r.push_back(char16_t((unsigned char)*s++)); return r; }
static void Check(const char* fmt, bool wide, std::vector<uint64_t> args, const char* expect) {
    Vec a; a.v = args;
    const std::u16string got = FormatGuest(U(fmt), wide, a);
    if (got != U(expect)) {
        std::string g; for (char16_t c : got) g.push_back(char(c));
        printf("ÉCHEC « %s » : obtenu « %s », attendu « %s »\n", fmt, g.c_str(), expect);
        g_fail++;
    }
}
int main() {
    g_base = (uint8_t*)platform::ReserveGuestMemory(0x100000000ull);
    const uint32_t narrow = 0x40000000, wide = 0x40000100;
    memcpy(Host(narrow), "Wakfu", 6);
    const char* w = "Yugo";
    for (int i = 0; i < 5; i++) St16(wide + 2 * i, uint16_t(w[i]));
    double d = 3.25; uint64_t dbits; memcpy(&dbits, &d, 8);
    Check("%d|%5d|%-4d|%05d", false, {uint64_t(-12), 42, 7, 3}, "-12|   42|7   |00003");
    Check("%u %x %X %08x", false, {0xFFFFFFFFull, 255, 255, 0xBEEF}, "4294967295 ff FF 0000beef");
    Check("%I64d %lld", false, {uint64_t(-5000000000ll), 1ull << 40}, "-5000000000 1099511627776");
    Check("%s/%S", false, {narrow, wide}, "Wakfu/Yugo");
    Check("%s/%S", true, {wide, narrow}, "Yugo/Wakfu");          // larges : %s UTF-16, %S étroit
    Check("%.3s|%6s|%-6s|", false, {narrow, narrow, narrow}, "Wak| Wakfu|Wakfu |");
    Check("%.2f %c %%", false, {dbits, 'x'}, "3.25 x %");
    Check("%*d|%.*s", false, {6, 9, 2, narrow}, "     9|Wa");
    printf(g_fail ? "%d échec(s)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
