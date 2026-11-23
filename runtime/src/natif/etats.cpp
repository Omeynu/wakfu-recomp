#include "natif/etats.h"

namespace wk::natif {

VkBlendFactor Facteur(uint32_t f) {
    static const VkBlendFactor t[17] = {
        VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE,   // 2, 3 : inutilisés
        VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
        VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_CONSTANT_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR, VK_BLEND_FACTOR_CONSTANT_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE};
    return f < 17 ? t[f] : VK_BLEND_FACTOR_ONE;
}
VkBlendOp Operation(uint32_t o) {
    static const VkBlendOp t[5] = {VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX,
                                   VK_BLEND_OP_REVERSE_SUBTRACT};
    return o < 5 ? t[o] : VK_BLEND_OP_ADD;
}
VkCompareOp Comparaison(uint32_t c) {
    static const VkCompareOp t[8] = {VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS, VK_COMPARE_OP_EQUAL,
                                     VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL,
                                     VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS};
    return t[c & 7];
}
VkStencilOp OpStencil(uint32_t o) {
    static const VkStencilOp t[8] = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE,
                                     VK_STENCIL_OP_INCREMENT_AND_CLAMP, VK_STENCIL_OP_DECREMENT_AND_CLAMP,
                                     VK_STENCIL_OP_INVERT, VK_STENCIL_OP_INCREMENT_AND_WRAP,
                                     VK_STENCIL_OP_DECREMENT_AND_WRAP};
    return t[o & 7];
}
bool Topologie(uint32_t prim, VkPrimitiveTopology* out) {
    switch (prim) {
        case 1: *out = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; return true;
        case 2: *out = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; return true;
        case 3: *out = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; return true;
        case 4: *out = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; return true;
        case 5: *out = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; return true;
        case 6: *out = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; return true;
        case 13: *out = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; return true;   // quads : 6 indices par quad (QuadIndices)
        default: return false;
    }
}
VkFormat FormatCible(uint32_t d3dformat, VkFormat d24s8) {
    switch (d3dformat & 0x3F) {
        case 4: return VK_FORMAT_R5G6B5_UNORM_PACK16;
        case 6: return VK_FORMAT_R8G8B8A8_UNORM;   // k_8_8_8_8 : la sortie du shader est (r, g, b, a)
        case 7: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case 21: case 26: case 32: return VK_FORMAT_R16G16B16A16_SFLOAT;   // shortcut: 16_16_16_16 fixe traité en flottant
        case 25: case 31: return VK_FORMAT_R16G16_SFLOAT;
        case 22: return d24s8;
        case 23: return VK_FORMAT_D32_SFLOAT_S8_UINT;   // k_24_8_FLOAT
        case 36: return VK_FORMAT_R32_SFLOAT;
        case 37: return VK_FORMAT_R32G32_SFLOAT;
        case 38: return VK_FORMAT_R32G32B32A32_SFLOAT;
        default: return VK_FORMAT_UNDEFINED;
    }
}
VkSampleCountFlagBits Echantillons(uint32_t ms) {
    return ms == 1 ? VK_SAMPLE_COUNT_2_BIT : ms == 2 ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT;
}

EtatPipeline TraduireEtat(const RegsEtat& r, bool avecProfondeur) {
    EtatPipeline e;
    const uint32_t b = r.blendControl;
    for (int i = 0; i < 4; i++) {
        VkPipelineColorBlendAttachmentState& a = e.blend[i];
        a.srcColorBlendFactor = Facteur(b & 31);
        a.colorBlendOp = Operation((b >> 5) & 7);
        a.dstColorBlendFactor = Facteur((b >> 8) & 31);
        a.srcAlphaBlendFactor = Facteur((b >> 16) & 31);
        a.alphaBlendOp = Operation((b >> 21) & 7);
        a.dstAlphaBlendFactor = Facteur((b >> 24) & 31);
        // ALPHABLENDENABLE = FALSE laisse ONE / ZERO / ADD dans le registre
        a.blendEnable = (b & 0x1FFF1FFF) != 0x00010001;
        a.colorWriteMask = (r.colorMask >> (4 * i)) & 0xF;   // bits R, G, B, A dans l'ordre de Vulkan
    }
    const uint32_t d = r.depthControl;
    VkPipelineDepthStencilStateCreateInfo& p = e.profondeur;
    if (avecProfondeur) {
        p.depthTestEnable = (d >> 1) & 1;
        p.depthWriteEnable = p.depthTestEnable && ((d >> 2) & 1);
        p.depthCompareOp = Comparaison(d >> 4);
        p.stencilTestEnable = d & 1;
        auto face = [](uint32_t func, uint32_t fail, uint32_t pass, uint32_t zfail, uint32_t refMask) {
            return VkStencilOpState{OpStencil(fail), OpStencil(pass), OpStencil(zfail), Comparaison(func),
                                    (refMask >> 8) & 0xFF, (refMask >> 16) & 0xFF, refMask & 0xFF};
        };
        p.front = face(d >> 8, d >> 11, d >> 14, d >> 17, r.stencilRefMask);
        p.back = (d >> 7) & 1 ? face(d >> 20, d >> 23, d >> 26, d >> 29, r.stencilRefMaskBF) : p.front;
    }
    const uint32_t m = r.modeControl;
    e.cull = (m & 1 ? VK_CULL_MODE_FRONT_BIT : 0) | (m & 2 ? VK_CULL_MODE_BACK_BIT : 0);
    e.face = (m >> 2) & 1 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    // poly_mode (bits 3-4) actif : type des faces avant (bits 5-7) ; 0 points, 1 lignes, 2 plein
    if ((m >> 3) & 3) {
        const uint32_t t = (m >> 5) & 7;
        e.polygone = t == 0 ? VK_POLYGON_MODE_POINT : t == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    }
    e.testAlpha = (r.colorControl >> 3) & 1;
    e.fonctionAlpha = Comparaison(r.colorControl);
    e.alphaVersCouverture = (r.colorControl >> 4) & 1;
    return e;
}

}  // namespace wk::natif
