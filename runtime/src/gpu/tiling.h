// Adressage tuilé 2D Xenos (macro-tuiles 32x32 éléments) ; un « élément » = texel ou bloc compressé.
// log2bpp = log2(octets par élément) ; widthElems = pas en éléments (multiple de 32).
#pragma once
#include <cstring>
#include <cstdint>

namespace wk::gpu {

inline uint32_t TiledOffset2DOuter(uint32_t y, uint32_t widthElems, uint32_t log2bpp) {
    const uint32_t macro = ((y >> 5) * (widthElems >> 5)) << (log2bpp + 7);
    const uint32_t micro = ((y & 6) << 2) << log2bpp;
    return macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 8) << (3 + log2bpp)) + ((y & 1) << 4);
}
inline uint32_t TiledOffset2DInner(uint32_t x, uint32_t y, uint32_t log2bpp, uint32_t base) {
    const uint32_t macro = (x >> 5) << (log2bpp + 7);
    const uint32_t micro = (x & 7) << log2bpp;
    const uint32_t off = base + macro + ((micro & ~0xFu) << 1) + (micro & 0xF);
    return ((off & ~0x1FFu) << 3) + ((off & 0x1C0) << 2) + (off & 0x3F) + ((y & 16) << 7) +
           (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
}
// Décalage en octets de l'élément (x, y)
inline uint32_t TiledOffset2D(uint32_t x, uint32_t y, uint32_t widthElems, uint32_t log2bpp) {
    return TiledOffset2DInner(x, y, log2bpp, TiledOffset2DOuter(y, widthElems, log2bpp));
}
// Mode d'octets Xenos appliqué à un mot lu en petit-boutiste par le GPU
inline uint32_t GpuEndianSwap(uint32_t v, uint32_t endian) {
    switch (endian & 3) {
        case 1: return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
        case 2: return __builtin_bswap32(v);
        case 3: return (v << 16) | (v >> 16);
        default: return v;
    }
}

}  // namespace wk::gpu
