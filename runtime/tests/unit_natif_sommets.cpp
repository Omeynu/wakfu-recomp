// Test : déclarations de sommets 360 -> formats Vulkan et conversion des octets (src/natif/sommets.cpp).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "guest.h"
#include "natif/sommets.h"
#include "platform.h"

using namespace natif;
static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("ÉCHEC %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

static void Elem(uint8_t* p, uint16_t stream, uint16_t off, uint32_t type, uint8_t usage, uint8_t idx) {
    const uint8_t e[12] = {uint8_t(stream >> 8), uint8_t(stream), uint8_t(off >> 8), uint8_t(off), uint8_t(type >> 24),
                           uint8_t(type >> 16), uint8_t(type >> 8), uint8_t(type), 0, usage, idx, 0};
    memcpy(p, e, 12);
}
static void End(uint8_t* p) { Elem(p, 0xFF, 0, kUnused, 0, 0); }

// Un élément seul au décalage 0 : format attendu, octets PC attendus (ou flottants si converti).
static void One(const char* name, uint32_t type, std::initializer_list<uint8_t> guest, VkFormat fmt,
                std::initializer_list<uint8_t> pc, std::initializer_list<float> conv = {}) {
    uint8_t decl[24];
    Elem(decl, 0, 0, type, kTexcoord, 3);
    End(decl + 12);
    const VertexLayout l = FromElements(decl);
    CHECK(l.ok && l.attrs.size() == 1, "%s : décodage", name);
    if (l.attrs.size() != 1) return;
    const Attribute& a = l.attrs[0];
    CHECK(a.format == fmt, "%s : format %d au lieu de %d", name, a.format, fmt);
    CHECK(a.usage == kTexcoord && a.usageIndex == 3, "%s : usage", name);
    CHECK(a.converted == (conv.size() != 0), "%s : converti %d", name, a.converted);
    const uint32_t stride = uint32_t(guest.size());
    uint8_t out[64] = {};
    ConvertVertices(l, 0, guest.begin(), stride, 1, out);
    if (!a.converted) {
        CHECK(memcmp(out, pc.begin(), pc.size()) == 0, "%s : octets %02X %02X %02X %02X", name, out[0], out[1], out[2], out[3]);
        return;
    }
    float f[4];
    memcpy(f, out + stride + a.extraOffset, 16);
    int i = 0;
    for (float want : conv) { CHECK(std::fabs(f[i] - want) < 1e-6f, "%s : composante %d = %g au lieu de %g", name, i, f[i], want); i++; }
}

int main() {
    // Flottants : 1.5 = 3FC00000, -2 = C0000000, 0.25 = 3E800000, 8 = 41000000
    One("FLOAT1", kFloat1, {0x3F, 0xC0, 0, 0}, VK_FORMAT_R32_SFLOAT, {0, 0, 0xC0, 0x3F});
    One("FLOAT2", kFloat2, {0x3F, 0xC0, 0, 0, 0xC0, 0, 0, 0}, VK_FORMAT_R32G32_SFLOAT, {0, 0, 0xC0, 0x3F, 0, 0, 0, 0xC0});
    One("FLOAT3", kFloat3, {0x3F, 0xC0, 0, 0, 0xC0, 0, 0, 0, 0x3E, 0x80, 0, 0}, VK_FORMAT_R32G32B32_SFLOAT,
        {0, 0, 0xC0, 0x3F, 0, 0, 0, 0xC0, 0, 0, 0x80, 0x3E});
    One("FLOAT4", kFloat4, {0x3F, 0xC0, 0, 0, 0xC0, 0, 0, 0, 0x3E, 0x80, 0, 0, 0x41, 0, 0, 0}, VK_FORMAT_R32G32B32A32_SFLOAT,
        {0, 0, 0xC0, 0x3F, 0, 0, 0, 0xC0, 0, 0, 0x80, 0x3E, 0, 0, 0, 0x41});
    // D3DCOLOR : mémoire A R G B (80 10 20 30) -> RGBA
    One("D3DCOLOR", kD3DColor, {0x80, 0x10, 0x20, 0x30}, VK_FORMAT_R8G8B8A8_UNORM, {0x10, 0x20, 0x30, 0x80});
    // UBYTE4* : mot gros-boutiste, X dans l'octet de poids faible (mémoire W Z Y X)
    One("UBYTE4", kUByte4, {4, 3, 2, 1}, VK_FORMAT_R8G8B8A8_USCALED, {1, 2, 3, 4});
    One("UBYTE4_2", kUByte4_2, {0xFC, 3, 2, 0xFF}, VK_FORMAT_R8G8B8A8_SSCALED, {0xFF, 2, 3, 0xFC});
    One("UBYTE4N", kUByte4N, {4, 3, 2, 0xFF}, VK_FORMAT_R8G8B8A8_UNORM, {0xFF, 2, 3, 4});
    One("UBYTE4N_2", kUByte4N_2, {0x81, 3, 2, 0x7F}, VK_FORMAT_R8G8B8A8_SNORM, {0x7F, 2, 3, 0x81});
    // 16 bits : chaque composante gros-boutiste
    One("SHORT2", kShort2, {0, 1, 0xFF, 0xFE}, VK_FORMAT_R16G16_SSCALED, {1, 0, 0xFE, 0xFF});
    One("SHORT4", kShort4, {0, 1, 0xFF, 0xFE, 0x12, 0x34, 0x80, 0}, VK_FORMAT_R16G16B16A16_SSCALED, {1, 0, 0xFE, 0xFF, 0x34, 0x12, 0, 0x80});
    One("SHORT2N", kShort2N, {0x7F, 0xFF, 0x80, 0x01}, VK_FORMAT_R16G16_SNORM, {0xFF, 0x7F, 0x01, 0x80});
    One("SHORT4N", kShort4N, {0x7F, 0xFF, 0x80, 0x01, 0, 0, 0, 1}, VK_FORMAT_R16G16B16A16_SNORM, {0xFF, 0x7F, 0x01, 0x80, 0, 0, 1, 0});
    One("USHORT2N", kUShort2N, {0xFF, 0xFF, 0x12, 0x34}, VK_FORMAT_R16G16_UNORM, {0xFF, 0xFF, 0x34, 0x12});
    One("USHORT4N", kUShort4N, {0xFF, 0xFF, 0x12, 0x34, 0, 2, 0, 3}, VK_FORMAT_R16G16B16A16_UNORM, {0xFF, 0xFF, 0x34, 0x12, 2, 0, 3, 0});
    One("FLOAT16_2", kFloat16_2, {0x3C, 0x00, 0xC0, 0x00}, VK_FORMAT_R16G16_SFLOAT, {0x00, 0x3C, 0x00, 0xC0});
    One("FLOAT16_4", kFloat16_4, {0x3C, 0x00, 0xC0, 0x00, 0x38, 0, 0, 0}, VK_FORMAT_R16G16B16A16_SFLOAT, {0x00, 0x3C, 0x00, 0xC0, 0, 0x38, 0, 0});
    // UDEC3 / DEC3N : X 0-9, Y 10-19, Z 20-29 ; W forcé à 1 par le swizzle (bits 30-31 réécrits)
    // UDEC3 X=5 Y=6 Z=7 W(bits)=0 : 0x00701805 -> W=1 : 0x40701805
    One("UDEC3", kUDec3, {0x00, 0x70, 0x18, 0x05}, VK_FORMAT_A2B10G10R10_USCALED_PACK32, {0x05, 0x18, 0x70, 0x40});
    // DEC3N X=511 Y=-511 (0x201) Z=0 W=0 : 0x000805FF -> W=1 (SNORM 2 bits : 1) : 0x400805FF
    One("DEC3N", kDec3N, {0x00, 0x08, 0x05, 0xFF}, VK_FORMAT_A2B10G10R10_SNORM_PACK32, {0xFF, 0x05, 0x08, 0x40});
    // UINT1 : entier 32 bits -> converti (256, 0, 0, 1)
    One("UINT1", kUInt1, {0, 0, 1, 0}, VK_FORMAT_R32G32B32A32_SFLOAT, {}, {256, 0, 0, 1});
    // DEC3N_2 : 10_11_11 signé normalisé, X 11 bits (1023 -> 1), Y 11 bits (-1023 -> -1), Z 10 bits (0) : 0x00200BFF
    // Swizzle de DEC3N_2 : XYZ1
    One("DEC3N_2", kDec3N_2, {0x00, 0x20, 0x0B, 0xFF}, VK_FORMAT_R32G32B32A32_SFLOAT, {}, {1, -1, 0, 1});
    // DEC3N_3 : 10_11_11 signé entier, X=-3 (0x7FD), Y=4, Z=-1 (0x3FF) : 0xFFC027FD
    One("DEC3N_3", kDec3N_3, {0xFF, 0xC0, 0x27, 0xFD}, VK_FORMAT_R32G32B32A32_SFLOAT, {}, {-3, 4, -1, 1});

    // Déclaration à deux flux, objet invité (FromDecl), Describe et pas PC avec attribut converti
    wk::g_base = (uint8_t*)wk::platform::ReserveGuestMemory(0x100000000ull);
    CHECK(wk::g_base, "mémoire invitée");
    if (!wk::g_base) return 1;
    const uint32_t decl = 0x40000000;
    uint8_t* e = wk::Host(decl + 0x34);
    Elem(e, 0, 0, kFloat3, kPosition, 0);
    Elem(e + 12, 0, 12, kDec3N_2, kNormal, 0);
    Elem(e + 24, 1, 0, kD3DColor, kColor, 0);
    End(e + 36);
    wk::St32(decl + 0x18, 3);
    const VertexLayout l = FromDecl(decl);
    CHECK(l.attrs.size() == 3 && l.streamMask == 3 && l.extra[0] == 16 && l.extra[1] == 0, "FromDecl");
    const uint32_t strides[16] = {16, 4};
    std::vector<VkVertexInputBindingDescription> b;
    std::vector<VkVertexInputAttributeDescription> at;
    Describe(l, strides, &b, &at);
    CHECK(b.size() == 2 && b[0].stride == 32 && b[1].binding == 1 && b[1].stride == 4, "bindings");
    CHECK(at.size() == 3 && at[1].location == 1 && at[1].offset == 16 && at[2].binding == 1 && at[0].offset == 0, "attributs");
    CHECK(StreamKey(l, 0) != StreamKey(l, 1), "empreintes de flux");

    // Positions en pixels (VTE sans échelle) -> NDC, VS à Y inversé : (0,0) -> (-1,1), (1280,720) -> (1,-1)
    {
        uint8_t px[2][32] = {};
        const float pts[2][3] = {{0, 0, 0.5f}, {1280, 720, 0.5f}};
        for (int v = 0; v < 2; v++) memcpy(px[v], pts[v], 12);
        PixelsVersNdc(l, 0, 16, 2, 1280, 720, &px[0][0]);
        float r[2][3];
        for (int v = 0; v < 2; v++) memcpy(r[v], px[v], 12);
        CHECK(r[0][0] == -1 && r[0][1] == 1 && r[1][0] == 1 && r[1][1] == -1 && r[0][2] == 0.5f, "pixels -> NDC");
    }

    // Indices 16 / 32 bits
    const uint8_t i16[4] = {0x12, 0x34, 0x00, 0x01}, w16[4] = {0x34, 0x12, 0x01, 0x00};
    const uint8_t i32[8] = {0, 0, 1, 2, 0xAA, 0xBB, 0xCC, 0xDD}, w32[8] = {2, 1, 0, 0, 0xDD, 0xCC, 0xBB, 0xAA};
    uint8_t o[8];
    ConvertIndices(i16, 4, false, o);
    CHECK(memcmp(o, w16, 4) == 0, "indices 16");
    ConvertIndices(i32, 8, true, o);
    CHECK(memcmp(o, w32, 8) == 0, "indices 32");

    // Quads -> triangles
    uint32_t q[12];
    QuadIndices(nullptr, 10, 2, q);
    const uint32_t wq[12] = {10, 11, 12, 10, 12, 13, 14, 15, 16, 14, 16, 17};
    CHECK(memcmp(q, wq, sizeof q) == 0, "quads non indexés");
    const uint32_t si[4] = {7, 3, 9, 5}, wi[6] = {7, 3, 9, 7, 9, 5};
    QuadIndices(si, 0, 1, q);
    CHECK(memcmp(q, wi, sizeof wi) == 0, "quads indexés");

    printf(g_fail ? "%d échec(s)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}
