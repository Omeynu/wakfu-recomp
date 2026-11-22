// Surveillance des écritures de la mémoire physique (platform::Watch*) : pages protégées, faute sur la vue principale
// et sur le miroir, numéro d'écriture, WatchTouch (GPU : protection gardée ; fichier : plage rendue inscriptible).
#include <cstdio>
#include <cstring>
#include "../src/platform.h"

static int g_fail = 0;
static void Check(bool ok, const char* what) { if (!ok) { printf("ÉCHEC : %s\n", what); g_fail++; } }

int main() {
    auto* base = static_cast<uint8_t*>(wk::platform::ReserveGuestMemory(0x100000000ull));
    if (!base) { printf("ÉCHEC : réservation\n"); return 1; }
    wk::platform::InstallCrashHandler(nullptr);
    wk::platform::WatchInit(base);
    const uint32_t a = 0x00100000, e = a + 3 * 4096;   // 3 pages physiques
    const uint64_t g0 = wk::platform::WatchGen(a, e);
    wk::platform::WatchProtect(a, e);
    Check(wk::platform::WatchGen(a, e) == g0, "protection sans écriture : numéro inchangé");
    const uint64_t s0 = wk::platform::WatchSerial();
    base[0xA0000000ull + a + 4096 + 5] = 42;            // faute -> page rendue inscriptible
    Check(wk::platform::WatchSerial() != s0, "faute : numéro global changé");
    const uint64_t g1 = wk::platform::WatchGen(a, e);
    Check(g1 > g0, "écriture (vue 0xA0000000) : numéro augmenté");
    Check(base[0xC0000000ull + a + 4096 + 5] == 42, "miroir 0xC0000000 : même mémoire");
    Check(wk::platform::WatchGen(a, a + 4096) == g0, "page voisine : numéro inchangé");
    base[0xA0000000ull + a + 4096 + 6] = 43;            // page déjà inscriptible : pas de faute
    Check(wk::platform::WatchGen(a, e) == g1, "seconde écriture sans protection : numéro inchangé");
    base[0xC0000000ull + a + 2 * 4096] = 7;             // faute par le miroir
    Check(wk::platform::WatchGen(a + 2 * 4096, e) > g1, "écriture (miroir) : numéro augmenté");
    wk::platform::WatchProtect(a, e);
    const uint64_t g2 = wk::platform::WatchGen(a, e);
    const uint64_t s1 = wk::platform::WatchSerial();
    wk::platform::WatchTouch(a, a + 4096, false);       // resolve GPU : numéro, protection gardée
    Check(wk::platform::WatchSerial() != s1, "WatchTouch : numéro global changé");
    Check(wk::platform::WatchGen(a, a + 4096) > g2, "WatchTouch : numéro augmenté");
    const uint64_t faults = wk::platform::WatchFaults();
    base[0xA0000000ull + a] = 1;
    Check(wk::platform::WatchFaults() == faults + 1, "WatchTouch sans `writable` : page toujours protégée");
    wk::platform::WatchProtect(a, e);
    wk::platform::WatchTouch(a, e, true);               // lecture de fichier : plage inscriptible, sans faute
    const uint64_t f2 = wk::platform::WatchFaults();
    memset(base + 0xA0000000ull + a, 9, e - a);
    Check(wk::platform::WatchFaults() == f2, "WatchTouch `writable` : aucune faute ensuite");
    printf("surveillance des écritures : %s\n", g_fail ? "ÉCHEC" : "OK");
    return g_fail ? 1 : 0;
}
