// Option [entrees] icones (voir icones.h). Formes dessinées ici, sans image : disques et traits suréchantillonnés
// (4 x 4 par pixel), palette prise aux icônes du jeu (disque cerné de sombre, étiquettes « parchemin »).
#include "natif/icones.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#define XXH_INLINE_ALL
#include <xxhash.h>

namespace wk::natif {

namespace {
std::atomic<bool> g_ps{false};

enum Forme { kCroix, kRond, kCarre, kTriangle, kL1, kR1, kL2, kR2, kShare, kOptions, kManette };
// Icônes Xbox du jeu (Media/XUI/IslandsOfWakfu.xzp, Images\...) : XXH3 64 des texels BGRA du niveau 0, taille
struct Connue { uint64_t empreinte; uint32_t w, h; Forme forme; };
constexpr Connue kConnues[] = {
    {0x0cfc64494b81541eull, 30, 30, kCroix},      // Skin\a.png (barres d'aide : « A Commencer la partie »)
    {0x7257ca3582d2e9ebull, 30, 30, kRond},       // Skin\b.png
    {0xba3eab66e29b8b1cull, 30, 30, kCarre},      // Skin\x.png
    {0x21945bc274e1c6e2ull, 30, 30, kTriangle},   // Skin\Y.png
    {0x80300c64a17c0652ull, 28, 28, kCroix},      // Html\A.png (pages d'aide)
    {0x7e96b20bc242bd8cull, 28, 28, kRond},       // Html\B.png
    {0x796195945fac98d4ull, 28, 28, kCarre},      // Html\X.png
    {0xa4eb904399f8bf5aull, 28, 28, kTriangle},   // Html\Y.png
    {0x0c230ed732585dbfull, 18, 18, kCroix},      // Html\miniA.png
    {0x2d9ec96c43fbf96cull, 50, 26, kL1},         // Skin\LB.png
    {0x8b92e06be1d591d6ull, 48, 26, kR1},         // Skin\RB.png
    {0x0b8c06ed589c05eeull, 60, 28, kL1},         // Html\LB.png
    {0x22de50739a8bd1b2ull, 60, 28, kR1},         // Html\RB.png
    {0xb1e01dceeb888204ull, 38, 28, kL2},         // Html\LT.png
    {0x7eb467c9cad2cf3cull, 38, 28, kR2},         // Html\RT.png
    {0xa0efd2ed2f0a331eull, 28, 28, kShare},      // Html\BACK.png
    {0xdf931db33f562199ull, 968, 496, kManette},  // Menu\Options\BackControls.png (schéma de la manette)
};

struct C { uint8_t r, g, b; };
constexpr C kCerne{45, 38, 32}, kCorps{78, 74, 70}, kReflet{98, 94, 89}, kBeige{220, 208, 175};
constexpr C kParchemin{228, 217, 184}, kBordParchemin{110, 95, 70}, kEncre{80, 66, 50};
constexpr C kCouleurs[4] = {{120, 170, 230}, {240, 95, 95}, {220, 140, 210}, {60, 205, 150}};   // croix, rond, carré, triangle

struct Img { uint8_t* p; uint32_t w, h; };
// Peint `c` là où dedans(x, y) (coordonnées en pixels, continues), par-dessus (alpha non prémultiplié, BGRA).
template <class F> void Couche(Img im, float x0, float y0, float x1, float y1, C c, F dedans) {
    const int ix0 = std::max(0, int(std::floor(x0))), iy0 = std::max(0, int(std::floor(y0)));
    const int ix1 = std::min(int(im.w), int(std::ceil(x1))), iy1 = std::min(int(im.h), int(std::ceil(y1)));
    for (int y = iy0; y < iy1; y++)
        for (int x = ix0; x < ix1; x++) {
            int n = 0;
            for (int s = 0; s < 16; s++) n += dedans(x + (s % 4 + 0.5f) / 4, y + (s / 4 + 0.5f) / 4);
            if (!n) continue;
            uint8_t* p = im.p + (size_t(y) * im.w + x) * 4;
            const float as = n / 16.0f, ad = p[3] / 255.0f, ao = as + ad * (1 - as);
            auto mel = [&](uint8_t s, uint8_t d) { return uint8_t(std::lround((s * as + d * ad * (1 - as)) / ao)); };
            p[0] = mel(c.b, p[0]), p[1] = mel(c.g, p[1]), p[2] = mel(c.r, p[2]), p[3] = uint8_t(std::lround(ao * 255));
        }
}
float DistSegment(float px, float py, float ax, float ay, float bx, float by) {
    const float dx = bx - ax, dy = by - ay, l = dx * dx + dy * dy;
    const float t = l > 0 ? std::clamp(((px - ax) * dx + (py - ay) * dy) / l, 0.0f, 1.0f) : 0.0f;
    return std::hypot(px - ax - t * dx, py - ay - t * dy);
}
// Polyligne de n points (x, y) dans `pts`, épaisseur 2 * e
bool DansTrait(const float* pts, int n, float x, float y, float e) {
    for (int i = 0; i + 1 < n; i++)
        if (DistSegment(x, y, pts[2 * i], pts[2 * i + 1], pts[2 * i + 2], pts[2 * i + 3]) <= e) return true;
    return false;
}

// Police à traits pour L, R, 1, 2 : polylignes dans une case 0,7 x 1 (y vers le bas), 0 point = fin
struct Lettre { char c; float p[2][16]; int n[2]; };
constexpr Lettre kLettres[] = {
    {'L', {{0, 0, 0, 1, 0.6f, 1}}, {3, 0}},
    {'R', {{0, 1, 0, 0, 0.45f, 0, 0.62f, 0.06f, 0.68f, 0.25f, 0.62f, 0.44f, 0.45f, 0.5f, 0, 0.5f}, {0.32f, 0.5f, 0.68f, 1}}, {8, 2}},
    {'1', {{0.12f, 0.22f, 0.42f, 0, 0.42f, 1}}, {3, 0}},
    {'2', {{0.02f, 0.22f, 0.15f, 0.05f, 0.35f, 0, 0.55f, 0.05f, 0.65f, 0.22f, 0.58f, 0.42f, 0.02f, 1, 0.68f, 1}}, {8, 0}},
};
// Texte centré en (cx, cy), hauteur de lettre h
void Texte(Img im, const char* t, float cx, float cy, float h, float e, C c) {
    const int n = int(strlen(t));
    const float avance = 0.95f * h, larg = (n - 1) * avance + 0.68f * h;
    for (int i = 0; i < n; i++) {
        const Lettre* l = nullptr;
        for (const Lettre& k : kLettres) if (k.c == t[i]) l = &k;
        if (!l) continue;
        const float ox = cx - larg / 2 + i * avance, oy = cy - h / 2;
        float pts[2][16];
        for (int s = 0; s < 2; s++)
            for (int j = 0; j < 2 * l->n[s]; j++) pts[s][j] = (j % 2 ? oy : ox) + l->p[s][j] * h;
        Couche(im, ox - e, oy - e, ox + 0.7f * h + e, oy + h + e, c, [&](float x, float y) {
            return DansTrait(pts[0], l->n[0], x, y, e) || DansTrait(pts[1], l->n[1], x, y, e);
        });
    }
}

// Bouton rond (formes 0 à 3, Share, Options) de rayon r centré en (cx, cy)
void Disque(Img im, int forme, float cx, float cy, float r) {
    auto dans = [](float x, float y, float cx, float cy, float r) { return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r; };
    const bool clair = forme == kShare || forme == kOptions;
    const float x0 = cx - r - 1, y0 = cy - r - 1, x1 = cx + r + 1, y1 = cy + r + 1;
    Couche(im, x0, y0, x1, y1, kCerne, [&](float x, float y) { return dans(x, y, cx, cy, r); });
    const float rc = r * 0.84f;
    Couche(im, x0, y0, x1, y1, clair ? kBeige : kCorps, [&](float x, float y) { return dans(x, y, cx, cy, rc); });
    if (!clair)
        Couche(im, x0, y0, x1, y1, kReflet, [&](float x, float y) { return dans(x, y, cx, cy, rc) && dans(x, y, cx, cy - 0.3f * r, 0.62f * r); });
    float pts[10];
    int n = 0;
    const float a = 0.36f * r;
    switch (forme) {
    case kCroix: break;
    case kCarre: { const float s = 0.3f * r; const float q[] = {cx - s, cy - s, cx + s, cy - s, cx + s, cy + s, cx - s, cy + s, cx - s, cy - s}; std::copy(q, q + 10, pts); n = 5; break; }
    case kTriangle: {
        const float t = 0.44f * r, c = cy + 0.06f * r;
        const float q[] = {cx, c - t, cx + t * 0.866f, c + t * 0.5f, cx - t * 0.866f, c + t * 0.5f, cx, c - t};
        std::copy(q, q + 8, pts); n = 4; break;
    }
    default: break;
    }
    auto symbole = [&](float x, float y, float e) {
        switch (forme) {
        case kCroix: return DistSegment(x, y, cx - a, cy - a, cx + a, cy + a) <= e || DistSegment(x, y, cx - a, cy + a, cx + a, cy - a) <= e;
        case kRond: return std::fabs(std::hypot(x - cx, y - cy) - a) <= e;
        case kCarre: case kTriangle: return DansTrait(pts, n, x, y, e);
        case kShare: return DistSegment(x, y, cx, cy - 0.32f * r, cx, cy + 0.32f * r) <= e * 1.3f;
        default: {   // Options : trois traits
            for (float dy : {-0.28f, 0.0f, 0.28f})
                if (DistSegment(x, y, cx - 0.3f * r, cy + dy * r, cx + 0.3f * r, cy + dy * r) <= e * 0.8f) return true;
            return false;
        }
        }
    };
    const float e = std::max(0.12f * r, 0.75f);
    if (!clair) Couche(im, x0, y0, x1, y1, kCerne, [&](float x, float y) { return symbole(x, y, e + std::max(0.07f * r, 0.5f)); });
    Couche(im, x0, y0, x1, y1, clair ? kCerne : kCouleurs[forme], [&](float x, float y) { return symbole(x, y, e); });
}

// Étiquette « parchemin » (L1 R1 L2 R2) dans [x0, x1[ x [y0, y1[ ; texte en haut si l'étiquette est plus haute que large
void Etiquette(Img im, const char* t, float x0, float y0, float x1, float y1) {
    const float h = y1 - y0, w = x1 - x0, ray = std::min(w, h) * 0.3f;
    auto arrondi = [&](float x, float y, float m) {
        const float ax = x0 + m + ray, bx = x1 - m - ray, ay = y0 + m + ray, by = y1 - m - ray;
        const float dx = std::max({ax - x, 0.0f, x - bx}), dy = std::max({ay - y, 0.0f, y - by});
        return x >= x0 + m && x <= x1 - m && y >= y0 + m && y <= y1 - m && dx * dx + dy * dy <= ray * ray;
    };
    const float bord = std::max(1.0f, std::min(w, h) * 0.08f);
    Couche(im, x0, y0, x1, y1, kBordParchemin, [&](float x, float y) { return arrondi(x, y, 0); });
    Couche(im, x0, y0, x1, y1, kParchemin, [&](float x, float y) { return arrondi(x, y, bord); });
    const float th = std::min(h, w * 0.75f) * 0.55f;
    const float cy = h > w ? y0 + bord + th * 0.9f : (y0 + y1) / 2;
    Texte(im, t, (x0 + x1) / 2, cy, th, std::max(th * 0.11f, 0.6f), kEncre);
}

// Schéma de la manette (968 x 496) : boutons Xbox recouverts aux places mesurées sur l'image du jeu
void Schema(Img im) {
    Disque(im, kTriangle, 435.5f, 96.5f, 14), Disque(im, kCarre, 408, 124.5f, 14);
    Disque(im, kRond, 465, 124.5f, 14), Disque(im, kCroix, 435.5f, 152.5f, 14);
    Disque(im, kShare, 290, 128, 12.5f), Disque(im, kOptions, 369, 128, 12.5f);
    Etiquette(im, "L1", 199, 29, 235, 45), Etiquette(im, "R1", 428, 29, 464, 45);
    Etiquette(im, "L2", 281, 0, 307, 60), Etiquette(im, "R2", 355, 0, 382, 60);
    const int combo[4] = {kCroix, kCarre, kRond, kTriangle};   // RT + A, X, B, Y
    for (int i = 0; i < 4; i++) {
        Etiquette(im, "R2", 567, 288 + 40 * i, 596, 320 + 40 * i);
        Disque(im, combo[i], 632.5f, 303.5f + 40 * i, 12.5f);
    }
}
}  // namespace

void ChoisirIcones(bool playstation) { g_ps.store(playstation, std::memory_order_relaxed); }

void DessinerIcone(int forme, uint8_t* bgra, uint32_t w, uint32_t h, int x0, int y0, int x1, int y1) {
    const Img im{bgra, w, h};
    static const char* const kNoms[4] = {"L1", "R1", "L2", "R2"};
    if (forme >= kL1 && forme <= kR2) Etiquette(im, kNoms[forme - kL1], float(x0), float(y0), float(x1), float(y1));
    else Disque(im, forme, (x0 + x1) / 2.0f, (y0 + y1) / 2.0f, std::min(x1 - x0, y1 - y0) / 2.0f);
}

bool RemplacerIcone(const Disposition& d, uint8_t* dst, const uint32_t debut[]) {
    if (!g_ps.load(std::memory_order_relaxed) || d.format != 6 || d.conversion || !d.niveaux) return false;
    const uint32_t w = d.n[0].w, h = d.n[0].h;
    if (std::none_of(std::begin(kConnues), std::end(kConnues), [&](const Connue& c) { return c.w == w && c.h == h; })) return false;
    const uint64_t e = XXH3_64bits(dst + debut[0], size_t(w) * h * 4);
    const Connue* k = nullptr;
    for (const Connue& c : kConnues)
        if (c.empreinte == e && c.w == w && c.h == h) k = &c;
    if (!k) return false;
    uint8_t* p = dst + debut[0];
    if (k->forme == kManette) Schema({p, w, h});
    else {   // l'icône du jeu effacée, la nôtre dessinée dans son emprise (alpha > 128)
        int x0 = int(w), y0 = int(h), x1 = 0, y1 = 0;
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                if (p[(y * w + x) * 4 + 3] > 128) x0 = std::min(x0, int(x)), y0 = std::min(y0, int(y)), x1 = std::max(x1, int(x) + 1), y1 = std::max(y1, int(y) + 1);
        memset(p, 0, size_t(w) * h * 4);
        DessinerIcone(k->forme, p, w, h, x0, y0, x1, y1);
    }
    // mips : moyenne 2 x 2 prémultipliée du niveau précédent
    for (uint32_t l = 1; l < d.niveaux; l++) {
        const uint8_t* s = dst + debut[l - 1];
        uint8_t* o = dst + debut[l];
        const uint32_t sw = d.n[l - 1].w, sh = d.n[l - 1].h, ow = d.n[l].w, oh = d.n[l].h;
        for (uint32_t y = 0; y < oh; y++)
            for (uint32_t x = 0; x < ow; x++) {
                float c[4] = {};
                for (uint32_t j = 0; j < 4; j++) {
                    const uint8_t* q = s + (size_t(std::min(2 * y + j / 2, sh - 1)) * sw + std::min(2 * x + j % 2, sw - 1)) * 4;
                    for (int i = 0; i < 3; i++) c[i] += q[i] * q[3] / 255.0f;
                    c[3] += q[3];
                }
                uint8_t* r = o + (size_t(y) * ow + x) * 4;
                for (int i = 0; i < 3; i++) r[i] = c[3] > 0 ? uint8_t(std::lround(std::min(255.0f, c[i] * 255.0f / c[3]))) : 0;
                r[3] = uint8_t(std::lround(c[3] / 4));
            }
    }
    fprintf(stderr, "[icônes] icône %ux%u du jeu remplacée (PlayStation)\n", w, h);
    return true;
}

}  // namespace wk::natif
