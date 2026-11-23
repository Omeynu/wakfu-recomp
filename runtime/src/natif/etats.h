// B3 (cœur natif) : traduction des états D3D9 Xbox 360 en états de pipeline Vulkan. Fonctions pures (testées par
// wakfu_unit_natif_etats). Les valeurs viennent de l'ombre des registres Xenos du device invité (docs/natif-b0-draws.md
// §2) : la D3D 360 y range les énumérations D3D9 telles quelles (D3DBLEND, D3DCMPFUNC, D3DSTENCILOP, D3DBLENDOP).
#pragma once
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include <cstdint>

namespace wk::natif {

// Registres lus dans le device (décalages depuis dev)
struct RegsEtat {
    uint32_t depthControl = 0;     // 0x2934 RB_DEPTHCONTROL
    uint32_t blendControl = 0;     // 0x2938 RB_BLENDCONTROL0
    uint32_t colorControl = 0;     // 0x293C RB_COLORCONTROL (test alpha : à faire dans le pixel shader)
    uint32_t modeControl = 0;      // 0x2948 PA_SU_SC_MODE_CNTL
    uint32_t colorMask = 0;        // 0x28DC RB_COLOR_MASK (4 bits par cible)
    uint32_t stencilRefMask = 0;   // 0x2900 RB_STENCILREFMASK
    uint32_t stencilRefMaskBF = 0; // 0x28FC RB_STENCILREFMASK_BF
};

struct EtatPipeline {
    VkPipelineColorBlendAttachmentState blend[4]{};
    VkPipelineDepthStencilStateCreateInfo profondeur{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkCullModeFlags cull = VK_CULL_MODE_NONE;
    VkFrontFace face = VK_FRONT_FACE_CLOCKWISE;
    VkPolygonMode polygone = VK_POLYGON_MODE_FILL;
    bool testAlpha = false;           // passé au pixel shader (pas d'état Vulkan)
    VkCompareOp fonctionAlpha = VK_COMPARE_OP_ALWAYS;
    bool alphaVersCouverture = false;
};

VkBlendFactor Facteur(uint32_t d3dblend);     // D3DBLEND 360 (ZERO 0, ONE 1, SRCCOLOR 4 … SRCALPHASAT 16)
VkBlendOp Operation(uint32_t d3dblendop);     // ADD 0, SUBTRACT 1, MIN 2, MAX 3, REVSUBTRACT 4
VkCompareOp Comparaison(uint32_t d3dcmp);     // NEVER 0 … ALWAYS 7
VkStencilOp OpStencil(uint32_t d3dop);        // KEEP 0 … DECR 7
// D3DPRIMITIVETYPE 360 -> topologie ; faux pour les rectangles (8) et quads (13), non pris en charge.
bool Topologie(uint32_t prim, VkPrimitiveTopology* out);
// D3DFORMAT 360 d'une surface/texture (format GPU = bits 0-5) -> format Vulkan ; UNDEFINED si inconnu.
// `d24s8` : format de profondeur choisi pour k_24_8 (D24S8 si pris en charge, sinon D32S8).
VkFormat FormatCible(uint32_t d3dformat, VkFormat d24s8 = VK_FORMAT_D24_UNORM_S8_UINT);
VkSampleCountFlagBits Echantillons(uint32_t d3dmultisample);   // NONE 0, 2_SAMPLES 1, 4_SAMPLES 2
// Convention : repère D3D (viewport de hauteur négative, ou positive avec Y inversé par le VS : même sens des faces).
EtatPipeline TraduireEtat(const RegsEtat& r, bool avecProfondeur);

}  // namespace wk::natif
