// Décodage CPU d'une texture 2D Xenos décrite par sa constante de lecture (6 mots) -> RGBA8.
#define XXH_INLINE_ALL
#include <xxhash.h>
#include <algorithm>
#include "texture_decode.h"
#include <cstring>
#include "../guest.h"
#include "tiling.h"

namespace wk::gpu {

namespace {
uint8_t Expand(uint32_t v, uint32_t bits) { return uint8_t(bits >= 8 ? v >> (bits - 8) : (v * 255 + ((1u << bits) - 1) / 2) / ((1u << bits) - 1)); }

// Bloc couleur DXT (8 octets, petit-boutiste après correction d'ordre) -> 16 texels RGBA
void DecodeDxtColor(const uint8_t* b, uint8_t out[16][4], bool dxt1) {
    const uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
    uint8_t pal[4][4];
    auto rgb = [](uint16_t c, uint8_t* p) { p[0] = Expand(c >> 11, 5); p[1] = Expand((c >> 5) & 63, 6); p[2] = Expand(c & 31, 5); p[3] = 255; };
    rgb(c0, pal[0]);
    rgb(c1, pal[1]);
    for (int k = 0; k < 3; k++) {
        if (!dxt1 || c0 > c1) {
            pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3);
        } else {
            pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2);
            pal[3][k] = 0;
        }
    }
    pal[2][3] = 255;
    pal[3][3] = (dxt1 && c0 <= c1) ? 0 : 255;
    const uint32_t idx = uint32_t(b[4]) | uint32_t(b[5]) << 8 | uint32_t(b[6]) << 16 | uint32_t(b[7]) << 24;
    for (int i = 0; i < 16; i++) memcpy(out[i], pal[(idx >> (2 * i)) & 3], 4);
}
void DecodeDxt5Alpha(const uint8_t* b, uint8_t out[16][4]) {
    uint8_t a[8] = {b[0], b[1]};
    if (a[0] > a[1]) for (int i = 1; i < 7; i++) a[i + 1] = uint8_t(((7 - i) * a[0] + i * a[1]) / 7);
    else { for (int i = 1; i < 5; i++) a[i + 1] = uint8_t(((5 - i) * a[0] + i * a[1]) / 5); a[6] = 0; a[7] = 255; }
    uint64_t bits = 0;
    for (int i = 0; i < 6; i++) bits |= uint64_t(b[2 + i]) << (8 * i);
    for (int i = 0; i < 16; i++) out[i][3] = a[(bits >> (3 * i)) & 7];
}
}  // namespace

TextureInfo ParseTextureFetch(const uint32_t f[6]) {
    TextureInfo t;
    t.type = f[0] & 3;
    t.pitch = ((f[0] >> 22) & 0x1FF) * 32;
    t.tiled = (f[0] >> 31) & 1;
    t.format = f[1] & 0x3F;
    t.endian = (f[1] >> 6) & 3;
    t.address = f[1] & 0xFFFFF000u;
    t.width = (f[2] & 0x1FFF) + 1;
    t.height = ((f[2] >> 13) & 0x1FFF) + 1;
    t.swizzle = (f[3] >> 1) & 0xFFF;
    t.dimension = (f[5] >> 9) & 3;
    return t;
}

// Empreinte des données sources (plage mémoire lue par DecodeTexture) : permet de garder une texture décodée d'une
// image à l'autre tant que la mémoire invitée n'a pas changé. 0 si format non géré.
uint64_t TextureSourceSpan(const TextureInfo& t) {
    uint32_t bpe = 0, block = 1;
    switch (t.format) {
        case 2: bpe = 1; break;
        case 3: case 4: case 10: case 15: bpe = 2; break;
        case 6: case 22: case 23: bpe = 4; break;
        case 18: bpe = 8; block = 4; break;
        case 19: case 20: bpe = 16; block = 4; break;
        default: return 0;
    }
    const uint32_t ew = (t.width + block - 1) / block, eh = (t.height + block - 1) / block;
    uint32_t pitchElems = (t.pitch ? t.pitch : t.width) / block;
    if (t.tiled) pitchElems = (pitchElems + 31) & ~31u;
    const uint32_t rows = t.tiled ? (eh + 31) & ~31u : eh;
    // tuilé 8 et 16 bits : la dernière rangée de macro-tuiles déborde de pas x lignes x bpe (au plus 1536 / 1024 octets,
    // relevé exhaustivement pour des pas de 32 à 2048 éléments)
    const uint32_t tail = t.tiled ? (bpe == 1 ? 1536 : bpe == 2 ? 1024 : 0) : 0;
    return uint64_t(std::max(pitchElems, ew)) * rows * bpe + tail;
}

uint64_t TextureSourceHash(const TextureInfo& t) {
    const uint64_t span = TextureSourceSpan(t);
    const uint32_t base = t.address < 0x20000000 ? 0xA0000000u | t.address : t.address;
    if (span == 0 || uint64_t(base) + span > 0x100000000ull) return 0;
    // XXH3 (xxHash de XenonRecomp, en-tête seul) : empreinte recalculée à chaque usage de la texture, poste
    // principal du thread du CP dans les scènes chargées (≈ 65 % des échantillons avec l'ancienne empreinte).
    const uint64_t h = XXH3_64bits_withSeed(Host(base), size_t(span), (uint64_t(t.format) << 32) ^ t.endian);
    return h ? h : 1;
}

// Textures de profondeur k_24_8 (22) / k_24_8_FLOAT (23) : mot de 32 bits, profondeur sur les 24 bits hauts
// (entier normalisé ; le format flottant est approché de la même façon) -> un flottant par texel (R32_SFLOAT).
bool DecodeDepthTexture(const TextureInfo& t, std::vector<float>* out) {
    if ((t.format != 22 && t.format != 23) || t.dimension != 1) return false;
    const uint32_t w = t.width, h = t.height;
    uint32_t pitch = t.pitch ? t.pitch : w;
    if (t.tiled) pitch = (pitch + 31) & ~31u;
    const uint32_t base = t.address < 0x20000000 ? 0xA0000000u | t.address : t.address;
    out->assign(size_t(w) * h, 0.0f);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            const uint32_t off = t.tiled ? TiledOffset2D(x, y, pitch, 2) : (y * pitch + x) * 4;
            uint32_t v;
            memcpy(&v, Host(base + off), 4);
            v = GpuEndianSwap(v, t.endian);
            (*out)[size_t(y) * w + x] = float(v >> 8) / 16777215.0f;
        }
    return true;
}

bool DecodeTexture(const TextureInfo& t, std::vector<uint8_t>* rgba, std::string* why) {
    // éléments : texels ou blocs 4x4 ; octets par élément
    uint32_t bpe = 0, block = 1;
    switch (t.format) {
        case 2: bpe = 1; break;                      // k_8
        case 3: case 4: case 10: case 15: bpe = 2; break;   // 1_5_5_5, 5_6_5, 8_8, 4_4_4_4
        case 6: bpe = 4; break;                      // k_8_8_8_8
        case 18: bpe = 8; block = 4; break;          // DXT1
        case 19: case 20: bpe = 16; block = 4; break;   // DXT2/3, DXT4/5
        default:
            if (why) *why = "format de texture " + std::to_string(t.format) + " non géré";
            return false;
    }
    if (t.dimension != 1) { if (why) *why = "dimension non 2D"; return false; }
    const uint32_t w = t.width, h = t.height;
    const uint32_t ew = (w + block - 1) / block, eh = (h + block - 1) / block;
    uint32_t pitchElems = (t.pitch ? t.pitch : w) / block;
    if (t.tiled) pitchElems = (pitchElems + 31) & ~31u;
    const uint32_t log2bpe = bpe == 1 ? 0 : bpe == 2 ? 1 : bpe == 4 ? 2 : bpe == 8 ? 3 : 4;
    const uint32_t base = t.address < 0x20000000 ? 0xA0000000u | t.address : t.address;
    std::vector<uint8_t> raw(size_t(ew) * eh * 4);   // texels « xyzw » avant swizzle
    raw.assign(size_t(w) * h * 4, 0);
    uint8_t elem[16];
    for (uint32_t ey = 0; ey < eh; ey++)
        for (uint32_t ex = 0; ex < ew; ex++) {
            const uint32_t off = t.tiled ? TiledOffset2D(ex, ey, pitchElems, log2bpe) : (ey * pitchElems + ex) * bpe;
            // lecture en vue GPU (petit-boutiste) + mode d'octets, mot par mot (ou demi-mot pour 8 dans 16)
            if (bpe < 4) {   // texel de 1 ou 2 octets : mode d'octets appliqué au mot de 32 bits qui le contient
                uint32_t v;
                memcpy(&v, Host(base + (off & ~3u)), 4);
                v = GpuEndianSwap(v, t.endian);
                uint8_t word[4];
                memcpy(word, &v, 4);
                memcpy(elem, word + (off & 3), bpe);
            } else {
                const uint8_t* src = Host(base + off);
                for (uint32_t i = 0; i < bpe; i += 4) {
                    uint32_t v;
                    memcpy(&v, src + i, 4);
                    v = GpuEndianSwap(v, t.endian);
                    memcpy(elem + i, &v, 4);
                }
            }
            if (block == 4) {
                uint8_t px[16][4];
                if (t.format == 18) DecodeDxtColor(elem, px, true);
                else {
                    DecodeDxtColor(elem + 8, px, false);
                    if (t.format == 20) DecodeDxt5Alpha(elem, px);
                    else for (int i = 0; i < 16; i++) px[i][3] = Expand((elem[i / 2] >> (4 * (i & 1))) & 15, 4);
                }
                for (int i = 0; i < 16; i++) {
                    const uint32_t x = ex * 4 + (i & 3), y = ey * 4 + (i >> 2);
                    if (x < w && y < h) memcpy(&raw[(size_t(y) * w + x) * 4], px[i], 4);
                }
                continue;
            }
            uint8_t* p = &raw[(size_t(ey) * w + ex) * 4];
            const uint32_t v = uint32_t(elem[0]) | uint32_t(elem[1]) << 8 | uint32_t(elem[2]) << 16 | uint32_t(elem[3]) << 24;
            switch (t.format) {
                case 2: p[0] = elem[0]; p[1] = p[2] = 0; p[3] = 255; break;
                case 10: p[0] = elem[0]; p[1] = elem[1]; p[2] = 0; p[3] = 255; break;
                case 4: p[0] = Expand(v & 31, 5); p[1] = Expand((v >> 5) & 63, 6); p[2] = Expand((v >> 11) & 31, 5); p[3] = 255; break;
                case 3: p[0] = Expand(v & 31, 5); p[1] = Expand((v >> 5) & 31, 5); p[2] = Expand((v >> 10) & 31, 5); p[3] = (v >> 15) & 1 ? 255 : 0; break;
                case 15: p[0] = Expand(v & 15, 4); p[1] = Expand((v >> 4) & 15, 4); p[2] = Expand((v >> 8) & 15, 4); p[3] = Expand((v >> 12) & 15, 4); break;
                default: memcpy(p, elem, 4);   // 8_8_8_8 : x = octet 0
            }
        }
    // swizzle de la constante : 3 bits par composante de sortie (0-3 = x-w, 4 = 0, 5 = 1)
    rgba->resize(size_t(w) * h * 4);
    for (size_t i = 0; i < size_t(w) * h; i++) {
        const uint8_t* s = &raw[i * 4];
        uint8_t* d = &(*rgba)[i * 4];
        for (int c = 0; c < 4; c++) {
            const uint32_t sel = (t.swizzle >> (3 * c)) & 7;
            d[c] = sel < 4 ? s[sel] : sel == 4 ? 0 : 255;
        }
    }
    return true;
}

// Un élément (texel, ou bloc 4x4 pour DXT) par invocation ; mêmes calculs entiers que DecodeTexture (Expand, palettes
// DXT, swizzle) puis écriture en unorm8 (k / 255 converti en k exactement). Profondeur : même flottant que
// float(v >> 8) / 16777215.0f, swizzle de la vue CPU (x, y, z -> profondeur ; 0 -> 0 ; sinon 1).
const char* const kTexDecodeGlsl = R"(
layout(local_size_x = 8, local_size_y = 8) in;
layout(std430, set = 0, binding = 0) readonly buffer Mem { uint w[]; } mem;
#ifdef DEPTH
layout(set = 0, binding = 1, rgba32f) uniform writeonly image2D dst;
#else
layout(set = 0, binding = 1, rgba8) uniform writeonly image2D dst;
#endif
layout(push_constant) uniform P { uint baseWord, pitch, w, h, endian, tiledFlag, swz, format; } p;
uint tiledOuter(uint y, uint width, uint l) {
    uint macro = ((y >> 5) * (width >> 5)) << (l + 7);
    uint micro = ((y & 6u) << 2) << l;
    return macro + ((micro & ~0xFu) << 1) + (micro & 0xFu) + ((y & 8u) << (3 + l)) + ((y & 1u) << 4);
}
uint tiled(uint x, uint y, uint width, uint l) {
    uint macro = (x >> 5) << (l + 7);
    uint micro = (x & 7u) << l;
    uint off = tiledOuter(y, width, l) + macro + ((micro & ~0xFu) << 1) + (micro & 0xFu);
    return ((off & ~0x1FFu) << 3) + ((off & 0x1C0u) << 2) + (off & 0x3Fu) + ((y & 16u) << 7) +
           (((((y & 8u) >> 2) + (x >> 3)) & 3u) << 6);
}
uint endianSwap(uint v, uint e) {
    if (e == 1u) return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    if (e == 2u) return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    if (e == 3u) return (v << 16) | (v >> 16);
    return v;
}
// mot de 32 bits contenant l'octet `off` de la texture, mode d'octets appliqué
uint word(uint off) {
    uint i = p.baseWord + (off >> 2);
    return i < uint(mem.w.length()) ? endianSwap(mem.w[i], p.endian) : 0u;
}
uint expand(uint v, uint bits) {
    return bits >= 8u ? v >> (bits - 8u) : (v * 255u + ((1u << bits) - 1u) / 2u) / ((1u << bits) - 1u);
}
uvec4 rgb565(uint c) { return uvec4(expand(c >> 11, 5u), expand((c >> 5) & 63u, 6u), expand(c & 31u, 5u), 255u); }
void store(uint x, uint y, uvec4 raw) {
    if (x >= p.w || y >= p.h) return;
#ifdef DEPTH
    // a / (2^24 - 1) = 0,aaa… en base 2^24 : arrondi au plus près = a * 2^-24 (exact) augmenté d'une unité de
    // dernier rang (bit suivant toujours 1, reste non nul) ; exact sur tout pilote, sans fma ni division
    uint a = raw.x;
    float q = a == 0u ? 0.0 : uintBitsToFloat(floatBitsToUint(float(a) * (1.0 / 16777216.0)) + 1u);
    vec4 o;
    for (int c = 0; c < 4; c++) {
        uint sel = (p.swz >> (3 * c)) & 7u;
        o[c] = sel < 3u ? q : (sel == 4u ? 0.0 : 1.0);
    }
#else
    vec4 o;
    for (int c = 0; c < 4; c++) {
        uint sel = (p.swz >> (3 * c)) & 7u;
        o[c] = sel < 4u ? float(raw[sel]) / 255.0 : (sel == 4u ? 0.0 : 1.0);
    }
#endif
    imageStore(dst, ivec2(x, y), o);
}
void main() {
    uvec2 e = gl_GlobalInvocationID.xy;
    uint f = p.format;
    bool dxt = f == 18u || f == 19u || f == 20u;
    uint bs = dxt ? 4u : 1u;
    if (e.x >= (p.w + bs - 1u) / bs || e.y >= (p.h + bs - 1u) / bs) return;
    uint l = f == 2u ? 0u : (f == 6u || f == 22u || f == 23u) ? 2u : f == 18u ? 3u : dxt ? 4u : 1u;
    uint off = p.tiledFlag != 0u ? tiled(e.x, e.y, p.pitch, l) : (e.y * p.pitch + e.x) << l;
#ifdef DEPTH
    store(e.x, e.y, uvec4(word(off) >> 8));
#else
    if (!dxt) {
        uint v = word(off & ~3u) >> (8u * (off & 3u));   // 1 ou 2 octets : dans le mot qui les contient
        uvec4 r;
        if (f == 2u) r = uvec4(v & 255u, 0u, 0u, 255u);
        else if (f == 10u) r = uvec4(v & 255u, (v >> 8) & 255u, 0u, 255u);
        else if (f == 4u) r = uvec4(expand(v & 31u, 5u), expand((v >> 5) & 63u, 6u), expand((v >> 11) & 31u, 5u), 255u);
        else if (f == 3u) r = uvec4(expand(v & 31u, 5u), expand((v >> 5) & 31u, 5u), expand((v >> 10) & 31u, 5u), ((v >> 15) & 1u) != 0u ? 255u : 0u);
        else if (f == 15u) r = uvec4(expand(v & 15u, 4u), expand((v >> 4) & 15u, 4u), expand((v >> 8) & 15u, 4u), expand((v >> 12) & 15u, 4u));
        else r = uvec4(v & 255u, (v >> 8) & 255u, (v >> 16) & 255u, v >> 24);
        store(e.x, e.y, r);
        return;
    }
    uint a0w = word(off), a1w = word(off + 4u);           // DXT2-5 : bloc alpha (8 premiers octets)
    uint cw0 = f == 18u ? a0w : word(off + 8u), cw1 = f == 18u ? a1w : word(off + 12u);
    uint c0 = cw0 & 0xFFFFu, c1 = cw0 >> 16;
    uvec4 pal[4];
    pal[0] = rgb565(c0);
    pal[1] = rgb565(c1);
    if (f != 18u || c0 > c1) {
        pal[2] = uvec4((2u * pal[0].rgb + pal[1].rgb) / 3u, 255u);
        pal[3] = uvec4((pal[0].rgb + 2u * pal[1].rgb) / 3u, 255u);
    } else {
        pal[2] = uvec4((pal[0].rgb + pal[1].rgb) / 2u, 255u);
        pal[3] = uvec4(0u);
    }
    uint a[8];
    a[0] = a0w & 255u;
    a[1] = (a0w >> 8) & 255u;
    if (a[0] > a[1]) for (uint i = 1u; i < 7u; i++) a[i + 1u] = ((7u - i) * a[0] + i * a[1]) / 7u;
    else { for (uint i = 1u; i < 5u; i++) a[i + 1u] = ((5u - i) * a[0] + i * a[1]) / 5u; a[6] = 0u; a[7] = 255u; }
    uint lo = (a0w >> 16) | (a1w << 16), hi = a1w >> 16;  // indices alpha DXT5 : 48 bits des octets 2 à 7
    for (uint i = 0u; i < 16u; i++) {
        uvec4 px = pal[(cw1 >> (2u * i)) & 3u];
        if (f == 19u) px.a = expand(((i < 8u ? a0w : a1w) >> (4u * (i & 7u))) & 15u, 4u);
        else if (f == 20u) {
            uint b = 3u * i;
            uint s = b < 32u ? (lo >> b) | (b > 29u ? hi << (32u - b) : 0u) : hi >> (b - 32u);
            px.a = a[s & 7u];
        }
        store(e.x * 4u + (i & 3u), e.y * 4u + (i >> 2), px);
    }
#endif
}
)";

bool TexDecodeSetup(const TextureInfo& t, uint32_t baseWord, TexDecodeParams* p, uint32_t* groupsX, uint32_t* groupsY) {
    uint32_t block = 1;
    switch (t.format) {
        case 2: case 3: case 4: case 6: case 10: case 15: case 22: case 23: break;
        case 18: case 19: case 20: block = 4; break;
        default: return false;
    }
    if (t.dimension != 1) return false;
    uint32_t pitch = (t.pitch ? t.pitch : t.width) / block;
    if (t.tiled) pitch = (pitch + 31) & ~31u;
    *p = {baseWord, pitch, t.width, t.height, t.endian & 3, t.tiled ? 1u : 0u, t.swizzle, t.format};
    *groupsX = ((t.width + block - 1) / block + 7) / 8;
    *groupsY = ((t.height + block - 1) / block + 7) / 8;
    return true;
}

}  // namespace wk::gpu
