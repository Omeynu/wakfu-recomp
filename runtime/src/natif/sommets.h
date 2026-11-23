// Rendu natif (B3) : déclarations de sommets D3D9 Xbox 360 -> entrées de sommets Vulkan, conversion des octets.
// Module autonome (aucun crochet) ; doc : docs/natif-b3-sommets.md.
#pragma once
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace natif {

// D3DDECLTYPE 360 = format de lecture Xenos (bits 0-5) | boutisme (6-7) | signé (8) | entier non normalisé (9)
// | swizzle 4 x 3 bits (10-21 ; 0-3 = X Y Z W, 4 = 0, 5 = 1).
enum DeclType : uint32_t {
    kFloat1 = 0x2C83A4, kFloat2 = 0x2C23A5, kFloat3 = 0x2A23B9, kFloat4 = 0x1A23A6,
    kD3DColor = 0x182886, kUByte4 = 0x1A2286, kUByte4_2 = 0x1A2386, kUByte4N = 0x1A2086, kUByte4N_2 = 0x1A2186,
    kShort2 = 0x2C2359, kShort4 = 0x1A235A, kShort2N = 0x2C2159, kShort4N = 0x1A215A,
    kUShort2N = 0x2C2059, kUShort4N = 0x1A205A, kUInt1 = 0x2C82A1,
    kUDec3 = 0x2A2287, kDec3N = 0x2A2187, kDec3N_2 = 0x2A2190, kDec3N_3 = 0x2A2390,
    kFloat16_2 = 0x2C235F, kFloat16_4 = 0x1A2360, kUnused = 0xFFFFFFFF,
};
enum DeclUsage : uint8_t {
    kPosition = 0, kBlendWeight, kBlendIndices, kNormal, kPSize, kTexcoord, kTangent, kBinormal,
    kTessFactor, kPositionT, kColor, kFog, kDepth, kSample,
};

struct Attribute {
    uint8_t stream = 0, usage = 0, usageIndex = 0;   // usage/index : relier aux entrées du shader
    uint16_t offset = 0;                              // dans le sommet invité
    uint32_t declType = 0;
    uint8_t size = 0;                                 // octets de l'élément invité
    VkFormat format = VK_FORMAT_UNDEFINED;            // format PC de l'attribut
    // Aucun format Vulkan, ou swizzle non réalisable par permutation d'octets (constante, format tassé permuté) : valeur recalculée en R32G32B32A32_SFLOAT (swizzle appliqué), ajoutée en fin de sommet PC.
    bool converted = false;
    uint16_t extraOffset = 0;                         // si converted : décalage après la fin du sommet invité
    uint8_t swizzle[4] = {0, 1, 2, 3};
};

struct VertexLayout {
    std::vector<Attribute> attrs;   // location Vulkan = indice dans attrs
    uint32_t streamMask = 0;
    uint16_t extra[16] = {};        // octets ajoutés par sommet PC, par flux
    uint64_t key = 0;               // empreinte des éléments (cache de pipelines, de tampons)
    bool ok = true;                 // faux : type inconnu (attribut ignoré)
};

// Éléments D3DVERTEXELEMENT9 360 gros-boutistes (12 o : Stream u16, Offset u16, Type u32, Method, Usage, UsageIndex, pad),
// fin = Stream 0xFF.
VertexLayout FromElements(const uint8_t* be, uint32_t maxCount = 64);
// Objet déclaration invité (CreateVertexDeclaration 821AC9A8) : n à +0x18, éléments à +0x34.
VertexLayout FromDecl(uint32_t guestDecl);

// Descriptions Vulkan (binding = flux), pas invité de chaque flux donné par SetStreamSource.
void Describe(const VertexLayout& l, const uint32_t guestStride[16],
              std::vector<VkVertexInputBindingDescription>* bindings,
              std::vector<VkVertexInputAttributeDescription>* attributes);
inline uint32_t PcStride(const VertexLayout& l, uint32_t stream, uint32_t guestStride) { return guestStride + l.extra[stream & 15]; }
// Empreinte des seuls attributs d'un flux (un VB est converti pour ce flux).
uint64_t StreamKey(const VertexLayout& l, uint32_t stream);

// Convertit `count` sommets invités du flux `stream` (src gros-boutiste, pas guestStride) vers dst
// (pas PcStride). Octets hors attributs : recopiés tels quels.
void ConvertVertices(const VertexLayout& l, uint32_t stream, const uint8_t* src, uint32_t guestStride,
                     uint32_t count, uint8_t* dst);
// Sommets en pixels de la cible (PA_CL_VTE_CNTL sans échelle X/Y, ex. vidéo XMV) : position flottante (après
// ConvertVertices) ramenée en NDC pour un VS qui la recopie (-fvk-invert-y : y = 1 - 2y/h), car Vulkan découpe avant
// le viewport ; w, h : taille de la cible.
void PixelsVersNdc(const VertexLayout& l, uint32_t stream, uint32_t guestStride, uint32_t count, float w, float h, uint8_t* dst);
// Indices : 16 ou 32 bits, boutisme 1 (8in16) ou 2 (8in32) ; bytes multiple de la taille d'indice.
void ConvertIndices(const uint8_t* src, uint32_t bytes, bool is32, uint8_t* dst);
// Liste de quads (D3DPT_QUADLIST 13) -> liste de triangles : 6 indices par quad (0,1,2 0,2,3) dans dst.
// `src` : indices PC du draw (nul : sommets first, first+1...) ; `quads` = nombre d'indices ou de sommets / 4.
void QuadIndices(const uint32_t* src, uint32_t first, uint32_t quads, uint32_t* dst);

}  // namespace natif
