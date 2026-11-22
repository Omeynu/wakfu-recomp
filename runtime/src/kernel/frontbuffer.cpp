// Capture du tampon d'image passé à VdSwap : constante de récupération de texture (6 mots),
// détuilage 2D Xenos, conversion en RGBA, PNG.
#include "frontbuffer.h"
#include <cstdio>
#include <vector>
#include "../guest.h"
#include "../png.h"
#include "../gpu/tiling.h"
#include "../gpu/texture_decode.h"

namespace wk {

namespace {
using gpu::GpuEndianSwap;
using gpu::TiledOffset2D;
}  // namespace

FrontBuffer ParseFetch(uint32_t fetchPtr) {
    uint32_t f[6];
    for (int i = 0; i < 6; i++) f[i] = Ld32(fetchPtr + 4 * i);
    return ParseFetchWords(f);
}

FrontBuffer ParseFetchWords(const uint32_t f[6]) {
    FrontBuffer fb;
    const uint32_t d0 = f[0], d1 = f[1], d2 = f[2];
    fb.pitch = ((d0 >> 22) & 0x1FF) * 32;
    fb.tiled = (d0 >> 31) & 1;
    fb.format = d1 & 0x3F;
    fb.endian = (d1 >> 6) & 3;
    fb.address = d1 & 0xFFFFF000u;
    fb.width = (d2 & 0x1FFF) + 1;
    fb.height = ((d2 >> 13) & 0x1FFF) + 1;
    return fb;
}

bool CaptureFrontBuffer(const uint32_t f[6], const std::string& path) {
    // Décodage par la constante de texture complète (format, mode d'octets, tuilage, swizzle)
    const gpu::TextureInfo t = gpu::ParseTextureFetch(f);
    std::vector<uint8_t> rgba;
    std::string why;
    if (!gpu::DecodeTexture(t, &rgba, &why)) { fprintf(stderr, "[capture] %s\n", why.c_str()); return false; }
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;   // tampon d'image : alpha ignoré
    return WritePng(path, t.width, t.height, rgba);
}

}  // namespace wk
