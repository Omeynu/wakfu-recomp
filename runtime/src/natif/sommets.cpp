// Déclarations de sommets 360 -> Vulkan et conversion des octets (cf. sommets.h).
#include "natif/sommets.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "guest.h"

namespace natif {
namespace {

// Formats de lecture Xenos (bits de chaque composante, à partir du bit 0 de l'élément remis en petit-boutiste)
// et formats Vulkan {UNORM, SNORM, USCALED, SSCALED} ; un seul format pour les flottants ; UNDEFINED = à convertir.
struct Fmt { uint8_t id, size, comps, bits[4]; bool isFloat; VkFormat vk[4]; };
constexpr VkFormat U = VK_FORMAT_UNDEFINED;
constexpr Fmt kFmts[] = {
    {6, 4, 4, {8, 8, 8, 8}, false, {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_USCALED, VK_FORMAT_R8G8B8A8_SSCALED}},
    {7, 4, 4, {10, 10, 10, 2}, false, {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_SNORM_PACK32,
                                         VK_FORMAT_A2B10G10R10_USCALED_PACK32, VK_FORMAT_A2B10G10R10_SSCALED_PACK32}},
    {16, 4, 3, {11, 11, 10}, false, {U, U, U, U}},   // 10_11_11 : aucun format Vulkan
    {17, 4, 3, {10, 11, 11}, false, {U, U, U, U}},   // 11_11_10
    {25, 4, 2, {16, 16}, false, {VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_USCALED, VK_FORMAT_R16G16_SSCALED}},
    {26, 8, 4, {16, 16, 16, 16}, false, {VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM,
                                           VK_FORMAT_R16G16B16A16_USCALED, VK_FORMAT_R16G16B16A16_SSCALED}},
    {31, 4, 2, {16, 16}, true, {VK_FORMAT_R16G16_SFLOAT}},
    {32, 8, 4, {16, 16, 16, 16}, true, {VK_FORMAT_R16G16B16A16_SFLOAT}},
    {33, 4, 1, {32}, false, {U, U, U, U}},           // entiers / normalisés 32 bits : pas de SCALED/NORM 32 bits en Vulkan
    {34, 8, 2, {32, 32}, false, {U, U, U, U}},
    {35, 16, 4, {32, 32, 32, 32}, false, {U, U, U, U}},
    {36, 4, 1, {32}, true, {VK_FORMAT_R32_SFLOAT}},
    {37, 8, 2, {32, 32}, true, {VK_FORMAT_R32G32_SFLOAT}},
    {38, 16, 4, {32, 32, 32, 32}, true, {VK_FORMAT_R32G32B32A32_SFLOAT}},
    {57, 12, 3, {32, 32, 32}, true, {VK_FORMAT_R32G32B32_SFLOAT}},
};
const Fmt* FindFmt(uint32_t id) {
    for (const Fmt& f : kFmts) if (f.id == id) return &f;
    return nullptr;
}

uint64_t Fnv(uint64_t h, const void* p, size_t n) {
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)p)[i]) * 0x100000001B3ull;
    return h;
}

void Swap(uint8_t* e, uint32_t size, uint32_t endian) {
    for (uint32_t i = 0; i + 4 <= size; i += 4) {
        uint8_t* b = e + i;
        if (endian == 1) { std::swap(b[0], b[1]); std::swap(b[2], b[3]); }
        else if (endian == 2) { std::swap(b[0], b[3]); std::swap(b[1], b[2]); }
        else if (endian == 3) { std::swap(b[0], b[2]); std::swap(b[1], b[3]); }
    }
}

float Half(uint32_t h) {
    const uint32_t e = (h >> 10) & 31, m = h & 1023;
    const float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? (m ? NAN : INFINITY) : std::ldexp(float(m | 1024), int(e) - 25);
    return (h & 0x8000) ? -v : v;
}

// Composante i (valeur flottante lue par le shader) d'un élément remis en petit-boutiste.
float Component(const Fmt& f, const uint8_t* e, int i, bool sign, bool integer) {
    uint32_t off = 0;
    for (int k = 0; k < i; k++) off += f.bits[k];
    const uint32_t w = f.bits[i];
    uint64_t raw = 0;
    memcpy(&raw, e + off / 8, (off % 8 + w + 7) / 8);
    raw = (raw >> (off % 8)) & ((1ull << w) - 1);
    if (f.isFloat) {
        if (w == 16) return Half(uint32_t(raw));
        float v; uint32_t r = uint32_t(raw); memcpy(&v, &r, 4); return v;
    }
    if (!sign) return integer ? float(raw) : float(double(raw) / double((1ull << w) - 1));
    const int64_t s = int64_t(raw << (64 - w)) >> (64 - w);
    return integer ? float(s) : float(std::max(-1.0, double(s) / double((1ull << (w - 1)) - 1)));
}

// Swizzle réalisable sans conversion : composantes permutées par octets (format aligné sur l'octet) ou identité,
// constantes 0/1 écrites dans la composante (W de DEC3N/UDEC3), remplissage Vulkan (0, 0, 1) au-delà de comps.
bool NativeSwizzle(const Fmt& f, const uint8_t swz[4]) {
    static const uint8_t fill[4] = {4, 4, 4, 5};
    const bool bytes = f.bits[0] % 8 == 0;
    for (int i = 0; i < 4; i++) {
        if (i >= f.comps) { if (swz[i] != fill[i]) return false; continue; }
        if (swz[i] == 4 || swz[i] == 5) continue;
        if (swz[i] >= f.comps || (swz[i] != i && !bytes)) return false;
    }
    return true;
}

// Écrit la constante 0 ou 1 (lue par le shader) dans la composante i de l'élément petit-boutiste e.
void SetConstant(const Fmt& f, uint8_t* e, int i, bool one, bool sign, bool integer) {
    uint32_t off = 0;
    for (int k = 0; k < i; k++) off += f.bits[k];
    const uint32_t w = f.bits[i];
    uint64_t v = 0;
    if (one) v = f.isFloat ? (w == 16 ? 0x3C00 : 0x3F800000) : integer ? 1 : sign ? (1ull << (w - 1)) - 1 : (1ull << w) - 1;
    const uint32_t n = (off % 8 + w + 7) / 8;
    uint64_t raw = 0;
    memcpy(&raw, e + off / 8, n);
    const uint64_t mask = ((1ull << w) - 1) << (off % 8);
    raw = (raw & ~mask) | (v << (off % 8) & mask);
    memcpy(e + off / 8, &raw, n);
}

}  // namespace

VertexLayout FromElements(const uint8_t* be, uint32_t maxCount) {
    VertexLayout l;
    l.key = 0xCBF29CE484222325ull;
    for (uint32_t n = 0; n < maxCount; n++, be += 12) {
        const uint16_t stream = uint16_t(be[0] << 8 | be[1]);
        if (stream == 0xFF) break;
        l.key = Fnv(l.key, be, 12);
        const uint32_t type = uint32_t(be[4]) << 24 | be[5] << 16 | be[6] << 8 | be[7];
        if (type == kUnused) continue;
        const Fmt* f = FindFmt(type & 63);
        if (!f || stream >= 16) { l.ok = false; continue; }
        Attribute a;
        a.stream = uint8_t(stream);
        a.offset = uint16_t(be[2] << 8 | be[3]);
        a.declType = type;
        a.usage = be[9];
        a.usageIndex = be[10];
        a.size = f->size;
        for (int i = 0; i < 4; i++) a.swizzle[i] = (type >> (10 + 3 * i)) & 7;
        const bool sign = type & 0x100, integer = type & 0x200;
        a.format = f->isFloat ? f->vk[0] : f->vk[(sign ? 1 : 0) + (integer ? 2 : 0)];
        if (a.format == VK_FORMAT_UNDEFINED || !NativeSwizzle(*f, a.swizzle)) {
            a.converted = true;
            a.format = VK_FORMAT_R32G32B32A32_SFLOAT;
            a.extraOffset = l.extra[stream];
            l.extra[stream] += 16;
        }
        l.streamMask |= 1u << stream;
        l.attrs.push_back(a);
    }
    return l;
}

VertexLayout FromDecl(uint32_t guestDecl) {
    if (!guestDecl) return VertexLayout{};
    return FromElements(wk::Host(guestDecl + 0x34), wk::Ld32(guestDecl + 0x18));
}

void Describe(const VertexLayout& l, const uint32_t guestStride[16], std::vector<VkVertexInputBindingDescription>* bindings,
              std::vector<VkVertexInputAttributeDescription>* attributes) {
    bindings->clear();
    attributes->clear();
    for (uint32_t s = 0; s < 16; s++)
        if (l.streamMask & (1u << s)) bindings->push_back({s, PcStride(l, s, guestStride[s]), VK_VERTEX_INPUT_RATE_VERTEX});
    for (uint32_t i = 0; i < l.attrs.size(); i++) {
        const Attribute& a = l.attrs[i];
        attributes->push_back({i, a.stream, a.format, a.converted ? guestStride[a.stream] + a.extraOffset : a.offset});
    }
}

uint64_t StreamKey(const VertexLayout& l, uint32_t stream) {
    uint64_t h = 0xCBF29CE484222325ull ^ stream;
    for (const Attribute& a : l.attrs)
        if (a.stream == stream) { h = Fnv(h, &a.offset, 2); h = Fnv(h, &a.declType, 4); }
    return h;
}

void ConvertVertices(const VertexLayout& l, uint32_t stream, const uint8_t* src, uint32_t guestStride, uint32_t count,
                     uint8_t* dst) {
    const uint32_t pcStride = PcStride(l, stream, guestStride);
    for (uint32_t v = 0; v < count; v++, src += guestStride, dst += pcStride) {
        memcpy(dst, src, guestStride);
        memset(dst + guestStride, 0, pcStride - guestStride);
        for (const Attribute& a : l.attrs) {
            if (a.stream != stream || a.offset + a.size > guestStride) continue;
            const Fmt& f = *FindFmt(a.declType & 63);
            uint8_t e[16];
            memcpy(e, src + a.offset, a.size);
            Swap(e, a.size, (a.declType >> 6) & 3);
            if (a.converted) {
                const bool sign = a.declType & 0x100, integer = a.declType & 0x200;
                float out[4];
                for (int i = 0; i < 4; i++) {
                    const uint8_t s = a.swizzle[i];
                    out[i] = s < f.comps ? Component(f, e, s, sign, integer) : s == 5 ? 1.0f : 0.0f;
                }
                memcpy(dst + guestStride + a.extraOffset, out, 16);
                continue;
            }
            uint8_t* d = dst + a.offset;
            const uint32_t cb = f.bits[0] % 8 ? 0 : f.bits[0] / 8;   // 0 : format tassé (swizzle identité)
            if (!cb) memcpy(d, e, a.size);
            for (int i = 0; cb && i < f.comps; i++)
                if (a.swizzle[i] < 4) memcpy(d + i * cb, e + a.swizzle[i] * cb, cb);
            for (int i = 0; i < f.comps; i++)
                if (a.swizzle[i] >= 4) SetConstant(f, d, i, a.swizzle[i] == 5, a.declType & 0x100, a.declType & 0x200);
        }
    }
}

void PixelsVersNdc(const VertexLayout& l, uint32_t stream, uint32_t guestStride, uint32_t count, float w, float h, uint8_t* dst) {
    const uint32_t pcStride = PcStride(l, stream, guestStride);
    for (const Attribute& a : l.attrs) {
        if (a.stream != stream || a.usage != kPosition || a.usageIndex != 0) continue;
        const bool flottant = a.converted || a.format == VK_FORMAT_R32G32_SFLOAT || a.format == VK_FORMAT_R32G32B32_SFLOAT ||
                              a.format == VK_FORMAT_R32G32B32A32_SFLOAT;
        if (!flottant || !w || !h) continue;
        const uint32_t off = a.converted ? guestStride + a.extraOffset : a.offset;
        for (uint32_t v = 0; v < count; v++) {
            float xy[2];
            memcpy(xy, dst + v * pcStride + off, 8);
            xy[0] = xy[0] * 2 / w - 1;
            xy[1] = 1 - xy[1] * 2 / h;
            memcpy(dst + v * pcStride + off, xy, 8);
        }
    }
}

void ConvertIndices(const uint8_t* src, uint32_t bytes, bool is32, uint8_t* dst) {
    const uint32_t n = is32 ? 4 : 2;
    for (uint32_t i = 0; i + n <= bytes; i += n)
        for (uint32_t k = 0; k < n; k++) dst[i + k] = src[i + n - 1 - k];
}

void QuadIndices(const uint32_t* src, uint32_t first, uint32_t quads, uint32_t* dst) {
    static const uint32_t kOrdre[6] = {0, 1, 2, 0, 2, 3};
    for (uint32_t q = 0; q < quads; q++)
        for (uint32_t k = 0; k < 6; k++) dst[6 * q + k] = src ? src[4 * q + kOrdre[k]] : first + 4 * q + kOrdre[k];
}

}  // namespace natif
