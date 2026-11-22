// Test des réglages de profil sur disque : écriture (binaire + entier), relecture, réglage inconnu non défini,
// fichier trop grand ignoré. Usage : wakfu_unit_profile <dossier temporaire>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include "../src/guest.h"
#include "../src/kernel/profile.h"
#include "../src/platform.h"
using namespace wk;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("ÉCHEC ligne %d : %s\n", __LINE__, #c); g_fail++; } } while (0)

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage : %s <dossier>\n", argv[0]); return 2; }
    const std::string dir = std::string(argv[1]) + "/profile";
    std::filesystem::remove_all(dir);
    g_base = (uint8_t*)platform::ReserveGuestMemory(0x100000000ull);
    const uint32_t kBin = 0x63E83FFF, kInt = 0x10040002, kUnset = 0x63E83FFE;   // binaire 1000 o, entier, binaire

    // écriture : 2 réglages
    const uint32_t s = 0x40000000, blob = 0x40001000;
    for (uint32_t i = 0; i < 80; i++) St8(s + i, 0);
    St32(s + 0x10, kBin); St8(s + 0x18, 0); St32(s + 0x20, 5); St32(s + 0x24, blob);   // type 0 comme le jeu
    memcpy(Host(blob), "WAKFU", 5);
    St32(s + 40 + 0x10, kInt); St8(s + 40 + 0x18, 1); St32(s + 40 + 0x20, 1234);
    CHECK(profile::Write(dir, 2, s) == 2);
    // binaire plus grand que le maximum de l'identifiant : refusé
    St32(s + 0x20, 1001);
    CHECK(profile::Write(dir, 1, s) == 0);

    // relecture : binaire, entier, non défini
    const uint32_t ids = 0x40002000, buf = 0x40010000;
    St32(ids, kBin); St32(ids + 4, kInt); St32(ids + 8, kUnset);
    const uint32_t need = profile::ReadSize(3, ids);
    CHECK(need == 8 + 3 * 40 + 2000);
    profile::Read(dir, 3, ids, buf, 0, 0xE000000000000001ull);
    CHECK(Ld32(buf) == 3 && Ld32(buf + 4) == buf + 8);
    const uint32_t e0 = buf + 8, e1 = e0 + 40, e2 = e1 + 40;
    CHECK(Ld32(e0) == 2 && Ld32(e0 + 0x10) == kBin && Ld8(e0 + 0x18) == 6 && Ld32(e0 + 0x20) == 5);
    CHECK(Ld32(e0 + 0x24) >= buf + 8 + 120 && Ld32(e0 + 0x24) + 5 <= buf + need);
    CHECK(memcmp(Host(Ld32(e0 + 0x24)), "WAKFU", 5) == 0);
    CHECK(Ld64(e0 + 8) == 0xE000000000000001ull);
    CHECK(Ld32(e1) == 2 && Ld8(e1 + 0x18) == 1 && Ld32(e1 + 0x20) == 1234);
    CHECK(Ld32(e2) == 0 && Ld32(e2 + 0x10) == kUnset && Ld8(e2 + 0x18) == 0);

    // fichier corrompu (binaire trop long) : ignoré, réglage non défini
    { std::ofstream f(dir + "/63E83FFE.bin", std::ios::binary); f.put(6); for (int i = 0; i < 1200; i++) f.put('x'); }
    profile::Read(dir, 3, ids, buf, 0, 0);
    CHECK(Ld32(buf + 8 + 80) == 0 && Ld32(buf + 8 + 8) == 0);

    std::filesystem::remove_all(dir);
    printf(g_fail ? "%d échec(s)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
