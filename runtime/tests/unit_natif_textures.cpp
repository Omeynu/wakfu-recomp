// N1 : détuilage et conversion du cache de textures natif (natif/textures.cpp) pour chaque format, tuilé ou non,
// chaque mode d'octets, plusieurs tailles et pas : empreintes du niveau 0 par format, relevées au commit 3674d14 quand
// ce test le comparait octet pour octet au décodage de référence de l'émulation (DecodeTexture, TextureBlocks,
// DecodeDepthTexture, retirés en N7) ; mise en page des mips (queue empaquetée) contre des valeurs calculées à la main
// d'après xenia texture_util.cc. Sans GPU.
#include <cstdio>
#include <iterator>
#include <cstring>
#include <random>
#include <vector>
#include "../src/guest.h"
#include "../src/natif/textures.h"

namespace wk { uint8_t* g_base = nullptr; }
// Fil natif, non utilisés ici (Disposer / Convertir n'en ont pas besoin)
namespace wk::natif { vk::Context* Contexte() { return nullptr; } VkCommandBuffer CmdEnvoi() { return VK_NULL_HANDLE; } }
using namespace wk::natif;

namespace {
constexpr uint32_t kMem = 32u << 20, kAdr = 0x00100000u, kMips = 0x00900000u;
uint32_t g_echecs = 0, g_cas = 0;

void Constante(uint32_t f[6], uint32_t fmt, uint32_t w, uint32_t h, uint32_t pitch, bool tuile, uint32_t endian,
               uint32_t mipMax = 0, bool empaquete = false) {
    f[0] = 2 | (pitch / 32) << 22 | uint32_t(tuile) << 31;
    f[1] = fmt | endian << 6 | kAdr;
    f[2] = (w - 1) | (h - 1) << 13;
    f[3] = 0x688 << 1;
    f[4] = mipMax << 6;
    f[5] = 1 << 9 | uint32_t(empaquete) << 11 | (mipMax ? kMips : 0);
}

// Niveau 0 converti, ajouté à l'empreinte FNV-1a du format
void Empreinte(uint64_t* h, uint32_t fmt, uint32_t w, uint32_t h0, uint32_t pitch, bool tuile, uint32_t endian) {
    uint32_t f[6];
    Constante(f, fmt, w, h0, pitch, tuile, endian);
    Disposition d;
    g_cas++;
    if (!Disposer(f, &d)) { if (g_echecs++ < 10) printf("ÉCHEC Disposer format %u %ux%u\n", fmt, w, h0); return; }
    std::vector<uint8_t> mien(TailleNiveau(d, 0));
    Convertir(d, 0, mien.data());
    for (uint8_t b : mien) *h = (*h ^ b) * 1099511628211ull;
}

void Attendu(const char* nom, uint32_t obtenu, uint32_t attendu) {
    g_cas++;
    if (obtenu != attendu && g_echecs++ < 30) printf("ÉCHEC %s : %u (attendu %u)\n", nom, obtenu, attendu);
}
}  // namespace

int main() {
    std::vector<uint8_t> mem(kMem);
    std::mt19937 rng(1234);
    for (auto& b : mem) b = uint8_t(rng());
    wk::g_base = mem.data() - 0xA0000000u;   // Host(0xA0000000 | adresse) -> mem[adresse]

    const uint32_t formats[] = {2, 3, 4, 6, 10, 15, 18, 19, 20, 22, 23};
    const uint64_t attendues[] = {0xAAD04F159894B13Dull, 0x434ACBB8F7DA1146ull, 0x7C0A0A9E4CF05EFCull, 0xA925544DDCA2B4CBull,
                                  0x83C58830341CD2C7ull, 0x3E7506AE614502CFull, 0xE07915A80708D58Bull, 0x1E2CB312C43B3C23ull,
                                  0x1E2CB312C43B3C23ull, 0xB79FA1E9C7A2656Aull, 0xB79FA1E9C7A2656Aull};
    const uint32_t tailles[][2] = {{1, 1}, {4, 4}, {13, 7}, {32, 32}, {64, 16}, {100, 60}, {256, 128}, {17, 300}};
    for (size_t i = 0; i < std::size(formats); i++) {
        const uint32_t fmt = formats[i];
        uint64_t h = 1469598103934665603ull;
        for (auto& s : tailles)
            for (int tuile = 0; tuile < 2; tuile++)
                for (uint32_t endian = 0; endian < 4; endian++) {
                    const uint32_t pitch = (s[0] + 31) & ~31u;
                    Empreinte(&h, fmt, s[0], s[1], pitch, tuile, endian);
                    if (!tuile) Empreinte(&h, fmt, s[0], s[1], pitch + 64, tuile, endian);   // pas plus grand que la largeur
                }
        // pas 0 (pas = largeur) : largeur impaire en k_8 / k_8_8 linéaire, rangées non alignées sur un mot (copie par mots exclue)
        if (fmt == 2 || fmt == 10)
            for (uint32_t endian = 0; endian < 4; endian++) { Empreinte(&h, fmt, 13, 7, 0, false, endian); Empreinte(&h, fmt, 1, 5, 0, false, endian); }
        g_cas++;
        if (h != attendues[i] && g_echecs++ < 10) printf("ÉCHEC format %u : empreinte %016llX\n", fmt, (unsigned long long)h);
    }
    printf("niveau 0 : %u cas\n", g_cas);

    // Mise en page des mips : DXT1 256x256 tuilée, 9 niveaux, queue empaquetée au niveau 4 (log2(256) - 4)
    uint32_t f[6];
    Disposition d;
    Constante(f, 18, 256, 256, 256, true, 2, 8, true);
    Attendu("DXT1 256 : Disposer", Disposer(f, &d), 1);
    Attendu("DXT1 256 : niveaux", d.niveaux, 9);
    const uint32_t m = 0xA0000000u | kMips;
    // niveaux 1-3 : 32 blocs de pas (128, 64, 32 texels arrondis à 32 blocs) x 32 rangées x 8 octets = 8 Ko chacun
    for (uint32_t l = 1; l <= 4; l++) Attendu("DXT1 256 : adresse", d.n[l].adresse, m + (l - 1) * 8192);
    for (uint32_t l = 5; l <= 8; l++) Attendu("DXT1 256 : adresse queue", d.n[l].adresse, m + 3 * 8192);
    for (uint32_t l = 1; l <= 8; l++) Attendu("DXT1 256 : pas", d.n[l].pas, 32);
    // queue (carré) : 16x16 en x = 16, 8x8 en x = 8, 4x4 en x = 4 texels ; 2x2 en y = 8, 1x1 en y = 4 (en blocs de 4)
    const uint32_t ox[] = {0, 0, 0, 0, 4, 2, 1, 0, 0}, oy[] = {0, 0, 0, 0, 0, 0, 0, 2, 1};
    for (uint32_t l = 0; l <= 8; l++) { Attendu("DXT1 256 : ox", d.n[l].ox, ox[l]); Attendu("DXT1 256 : oy", d.n[l].oy, oy[l]); }
    Attendu("DXT1 256 : taille 8", d.n[8].w, 1);

    // Petite texture 8888 16x8 linéaire empaquetée dès le niveau 0 : base en (0, 16) (plus large que haute), mips à
    // l'adresse des mips en (0, 8), (0, 4), puis 2 = 1 << (4 - 0) >> 1 = 8 en x
    Constante(f, 6, 16, 8, 32, false, 0, 4, true);
    Attendu("8888 16x8 : Disposer", Disposer(f, &d), 1);
    Attendu("8888 16x8 : niveaux", d.niveaux, 5);
    Attendu("8888 16x8 : adresse 0", d.n[0].adresse, 0xA0000000u | kAdr);
    Attendu("8888 16x8 : oy 0", d.n[0].oy, 16);
    Attendu("8888 16x8 : adresse 1", d.n[1].adresse, m);
    Attendu("8888 16x8 : pas 1", d.n[1].pas, 64);   // 32 texels x 4 octets alignés sur 256 octets
    Attendu("8888 16x8 : oy 1", d.n[1].oy, 8);
    Attendu("8888 16x8 : oy 2", d.n[2].oy, 4);
    Attendu("8888 16x8 : ox 3", d.n[3].ox, 8);
    Attendu("8888 16x8 : ox 4", d.n[4].ox, 4);

    // Sans queue empaquetée : 8888 64x32 linéaire, 3 niveaux ; niveau 1 : 32 texels -> 32 x 4 = 128 octets alignés 256
    // (pas 64 éléments), 16 rangées arrondies à 32 -> 8 Ko arrondis à 4 Ko = 8 Ko
    Constante(f, 6, 64, 32, 64, false, 0, 2, false);
    Attendu("8888 64x32 : Disposer", Disposer(f, &d), 1);
    Attendu("8888 64x32 : pas 1", d.n[1].pas, 64);
    Attendu("8888 64x32 : adresse 2", d.n[2].adresse, m + 8192);
    Attendu("8888 64x32 : taille 2", d.n[2].w * 100 + d.n[2].h, 1608);
    Attendu("8888 64x32 : ox 2", d.n[2].ox + d.n[2].oy, 0);

    // Non gérés : cube, format inconnu
    Constante(f, 6, 64, 64, 64, true, 0);
    f[5] = 3 << 9;
    Attendu("cube refusé", Disposer(f, &d), 0);
    Constante(f, 63, 64, 64, 64, true, 0);
    Attendu("format 63 refusé", Disposer(f, &d), 0);

    printf("%u cas, %u échecs : %s\n", g_cas, g_echecs, g_echecs ? "ÉCHEC" : "OK");
    return g_echecs ? 1 : 0;
}
