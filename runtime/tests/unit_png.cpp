// Test de PngVersIco : en-tête .ico correct autour du PNG, refus d'un fichier qui n'est pas un PNG ou trop grand.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
#include "../src/png.h"
using namespace wk;
static int g_fail = 0;
#define VERIF(c) do { if (!(c)) { printf("ÉCHEC ligne %d : %s\n", __LINE__, #c); g_fail++; } } while (0)
static std::vector<uint8_t> Lire(const char* p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
int main() {
    VERIF(WritePng("unit_png_64.png", 64, 64, std::vector<uint8_t>(64 * 64 * 4, 0x80)));
    VERIF(PngVersIco("unit_png_64.png", "unit_png_64.ico"));
    const auto png = Lire("unit_png_64.png"), ico = Lire("unit_png_64.ico");
    VERIF(ico.size() == png.size() + 22);
    if (ico.size() == png.size() + 22) {
        const uint8_t t[] = {0, 0, 1, 0, 1, 0, 64, 64, 0, 0, 1, 0, 32, 0};
        VERIF(std::equal(t, t + 14, ico.begin()));
        VERIF((ico[14] | ico[15] << 8 | ico[16] << 16 | ico[17] << 24) == int(png.size()));
        VERIF(ico[18] == 22 && ico[19] == 0);
        VERIF(std::equal(png.begin(), png.end(), ico.begin() + 22));
    }
    VERIF(WritePng("unit_png_256.png", 256, 256, std::vector<uint8_t>(256 * 256 * 4, 0)));
    VERIF(PngVersIco("unit_png_256.png", "unit_png_256.ico") && Lire("unit_png_256.ico")[6] == 0);   // 256 -> 0
    VERIF(WritePng("unit_png_300.png", 300, 1, std::vector<uint8_t>(300 * 4, 0)));
    VERIF(!PngVersIco("unit_png_300.png", "unit_png_300.ico"));
    VERIF(!PngVersIco("unit_png.cpp.absent", "x.ico"));
    std::ofstream("unit_png_faux.png") << "pas un png, assez long pour l'en-tête";
    VERIF(!PngVersIco("unit_png_faux.png", "x.ico"));
    {   // TexteRgba : taille, cadre sombre, pixel du « C » allumé, « é » sur 2 octets compté comme un caractère
        uint32_t w = 0, h = 0;
        const auto t = TexteRgba("C\xc3\xa9", 2, &w, &h);
        VERIF(w == 2 * 6 * 2 - 2 + 16 && h == 8 * 2 + 16 && t.size() == size_t(w) * h * 4);
        VERIF(t[0] == 24 && t[3] == 255);
        VERIF(t[((8 + 0) * w + 8 + 2) * 4] == 255);    // « C » ligne 0 : 01110 -> colonne 1 allumée (x 2..3)
        VERIF(t[((8 + 0) * w + 8 + 0) * 4] == 24);     // colonne 0 éteinte
        VERIF(t[((8 + 0) * w + 8 + 12 + 6) * 4] == 255);   // « é » ligne 0 : 00010 -> colonne 3 (x 6..7)
        const auto m = TexteRgba("Capture effectu\xc3\xa9" "e", 3, &w, &h);
        VERIF(WritePng("unit_png_texte.png", w, h, m));
    }
    printf(g_fail ? "unit_png : %d échec(s)\n" : "unit_png : OK\n", g_fail);
    return g_fail != 0;
}
