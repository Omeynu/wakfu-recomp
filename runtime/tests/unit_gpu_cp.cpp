// Test unitaire du processeur PM4 : écritures mémoire selon le mode d'octets, registres scratch,
// tampon indirect, anneau qui boucle.
#include <cstdio>
#include "../src/guest.h"
#include "../src/kernel/gpu_cp.h"
#include "../src/platform.h"

using namespace wk;
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "ÉCHEC ligne %d : %s\n", __LINE__, #c); g_fail++; } } while (0)

static uint32_t T3(uint32_t op, uint32_t n) { return 0xC0000000u | ((n - 1) << 16) | (op << 8); }

int main() {
    g_base = (uint8_t*)platform::ReserveGuestMemory(0x100000000ull);
    if (!g_base) { fprintf(stderr, "mémoire invitée indisponible\n"); return 2; }
    // Miroir physique : 0xC0000000 + x et 0xA0000000 + x sont la même mémoire.
    St32(0xA019BA00, 0x12345678);
    if (Ld32(0xC019BA00) != 0x12345678) { fprintf(stderr, "ÉCHEC miroir 0xC0000000\n"); return 1; }
    St32(0xDFFFFFFC, 0xCAFEF00D);
    if (Ld32(0xBFFFFFFC) != 0xCAFEF00D) { fprintf(stderr, "ÉCHEC miroir (fin)\n"); return 1; }
    const uint32_t ring = 0xA0004000, sizeDw = 64;   // mémoire physique 0x4000 vue en 0xA0004000
    uint32_t w = 0;
    auto put = [&](uint32_t v) { St32(ring + 4 * (w % sizeDw), v); w++; };
    // MEM_WRITE mode 2 (8 dans 32) : le CPU doit lire la valeur telle quelle
    put(T3(0x3D, 2)); put(0x00001000 | 2); put(0x12345678);
    // MEM_WRITE mode 0 : le CPU lit la valeur inversée
    put(T3(0x3D, 2)); put(0x00001004); put(0x12345678);
    // EVENT_WRITE_SHD avec compteur de swap (bit 31 de l'initiateur)
    put(T3(0x58, 3)); put(0x80000000); put(0x00001008 | 2); put(0xDEAD);
    // registres scratch : UMSK=1, ADDR=0x1100, SCRATCH_REG0=0xCAFE (paquet type 0)
    put(0x00000000u | 0x1DC); put(1);
    put(0x00000000u | 0x1DD); put(0x1100);
    put(0x00000000u | 0x578); put(0xCAFE);
    // tampon indirect en 0x2000 contenant un MEM_WRITE
    St32(0xA0002000, T3(0x3D, 2)); St32(0xA0002004, 0x0000100C | 2); St32(0xA0002008, 0x55);
    put(T3(0x3F, 2)); put(0x2000); put(3);
    CpSetSwapCounter(7);
    uint32_t rptr = CpProcessRing(ring, sizeDw, 0, w % sizeDw);
    CHECK(rptr == w % sizeDw);
    CHECK(Ld32(0xA0001000) == 0x12345678);
    CHECK(Ld32(0xA0001004) == 0x78563412);
    CHECK(Ld32(0xA0001008) == 7);
    CHECK(Ld32(0xA0001100) == 0xCAFE);
    CHECK(Ld32(0xA000100C) == 0x55);
    // anneau qui boucle : un paquet à cheval sur la fin
    w = rptr = sizeDw - 1;
    put(T3(0x3D, 2)); put(0x00001010 | 2); put(0x99);
    rptr = CpProcessRing(ring, sizeDw, rptr, w % sizeDw);
    CHECK(rptr == w % sizeDw);
    CHECK(Ld32(0xA0001010) == 0x99);
    // Constantes : SET_CONSTANT type 0 (ALU, index 2 -> 0x4002), LOAD_ALU_CONSTANT (PS c253 = 0x4000 + 509*4,
    // 4 mots lus en mémoire physique 0x3000), SET_CONSTANT type 4 (registres : 0x2000 + 0x201), SET_SHADER_CONSTANTS
    for (uint32_t i = 0; i < 4; i++) St32(0xA0003000 + 4 * i, 0x3F800000 + i);
    w = rptr;
    put(T3(0x2D, 3)); put(0x00000002); put(0x11); put(0x22);
    put(T3(0x2F, 3)); put(0x3000); put(0x00000000 | (509 * 4)); put(4);
    put(T3(0x2D, 2)); put(0x00040201); put(0x707);
    put(T3(0x56, 2)); put(0x4900); put(0xB0);
    rptr = CpProcessRing(ring, sizeDw, rptr, w % sizeDw);
    CHECK(CpRegister(0x4002) == 0x11 && CpRegister(0x4003) == 0x22);
    CHECK(CpRegister(0x4000 + 509 * 4) == 0x3F800000 && CpRegister(0x4000 + 509 * 4 + 3) == 0x3F800003);
    CHECK(CpRegister(0x2201) == 0x707);
    CHECK(CpRegister(0x4900) == 0xB0);
    // Prédication : masque 0x2, sélection 0x1 -> MEM_WRITE prédiqué sauté ; sélection 0x2 -> exécuté
    w = rptr;
    put(T3(0x60, 1)); put(0x2); put(T3(0x61, 1)); put(0);
    put(T3(0x51, 2)); put(0); put(0x1);
    put(T3(0x3D, 2) | 1); put(0x00001020 | 2); put(0x111);
    put(T3(0x62, 1)); put(0x2);
    put(T3(0x3D, 2) | 1); put(0x00001024 | 2); put(0x222);
    rptr = CpProcessRing(ring, sizeDw, rptr, w % sizeDw);
    CHECK(Ld32(0xA0001020) == 0 && Ld32(0xA0001024) == 0x222);
    printf(g_fail ? "%d échec(s)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
