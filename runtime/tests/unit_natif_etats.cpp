// Traduction des états D3D 360 -> Vulkan du rendu natif (B3) : valeurs attendues tirées de l'énumération D3D9.
#include <cstdio>
#include "natif/etats.h"

static int g_fail = 0;
#define VERIFIE(c) do { if (!(c)) { fprintf(stderr, "échec ligne %d : %s\n", __LINE__, #c); g_fail++; } } while (0)

int main() {
    using namespace wk::natif;
    // Tables
    VERIFIE(Facteur(6) == VK_BLEND_FACTOR_SRC_ALPHA && Facteur(7) == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
    VERIFIE(Facteur(16) == VK_BLEND_FACTOR_SRC_ALPHA_SATURATE && Facteur(13) == VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR);
    VERIFIE(Operation(4) == VK_BLEND_OP_REVERSE_SUBTRACT && Operation(9) == VK_BLEND_OP_ADD);
    VERIFIE(Comparaison(3) == VK_COMPARE_OP_LESS_OR_EQUAL && Comparaison(6) == VK_COMPARE_OP_GREATER_OR_EQUAL);
    VERIFIE(OpStencil(3) == VK_STENCIL_OP_INCREMENT_AND_CLAMP && OpStencil(6) == VK_STENCIL_OP_INCREMENT_AND_WRAP);
    VkPrimitiveTopology t;
    VERIFIE(Topologie(6, &t) && t == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
    VERIFIE(!Topologie(8, &t) && Topologie(13, &t) && t == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);   // quads : indices (sommets.cpp)
    VERIFIE(FormatCible(0x18280186) == VK_FORMAT_R8G8B8A8_UNORM);    // A8R8G8B8 de CreateDevice
    VERIFIE(FormatCible(0x2D200196) == VK_FORMAT_D24_UNORM_S8_UINT); // D24S8
    VERIFIE(FormatCible(0x2D200196, VK_FORMAT_D32_SFLOAT_S8_UINT) == VK_FORMAT_D32_SFLOAT_S8_UINT);
    VERIFIE(FormatCible(0x12) == VK_FORMAT_UNDEFINED);               // DXT1 : pas une cible
    VERIFIE(Echantillons(0) == VK_SAMPLE_COUNT_1_BIT && Echantillons(2) == VK_SAMPLE_COUNT_4_BIT);

    // Définition interne x2 : rectangle de resolve d'une bande de tuilage, viewport, point de destination
    int32_t rect[4] = {320, 0, 640, 720};
    AgrandirRect(rect, 2);
    VERIFIE(rect[0] == 640 && rect[1] == 0 && rect[2] == 1280 && rect[3] == 1440);
    float vp[6] = {0.5f, 0, 1280, 720, 0, 1};
    AgrandirViewport(vp, 2);
    VERIFIE(vp[0] == 1 && vp[2] == 2560 && vp[3] == 1440 && vp[4] == 0 && vp[5] == 1);
    VERIFIE(PointX(256u << 16 | 128, 2) == 512 && PointY(256u << 16 | 128, 2) == 256 && PointY(0xFFFF, 1) == 0xFFFF);

    // Défauts D3D : ZENABLE, ZWRITEENABLE, ZFUNC LESSEQUAL ; pas de mélange ; masque 0xF ; pas de culling
    RegsEtat r;
    r.depthControl = 0x36;
    r.blendControl = 0x00010001;
    r.colorMask = 0xF;
    EtatPipeline e = TraduireEtat(r, true);
    VERIFIE(e.profondeur.depthTestEnable && e.profondeur.depthWriteEnable);
    VERIFIE(e.profondeur.depthCompareOp == VK_COMPARE_OP_LESS_OR_EQUAL && !e.profondeur.stencilTestEnable);
    VERIFIE(!e.blend[0].blendEnable && e.blend[0].colorWriteMask == 0xF && e.blend[1].colorWriteMask == 0);
    VERIFIE(e.cull == VK_CULL_MODE_NONE && e.polygone == VK_POLYGON_MODE_FILL);
    VERIFIE(!TraduireEtat(r, false).profondeur.depthTestEnable);   // sans surface de profondeur

    // SRCALPHA / INVSRCALPHA / ADD (couleur), ONE / ONE / MAX (alpha) ; écriture R et A seulement
    r.blendControl = 6 | 0 << 5 | 7 << 8 | 1 << 16 | 3 << 21 | 1 << 24;
    r.colorMask = 0x9;
    e = TraduireEtat(r, true);
    VERIFIE(e.blend[0].blendEnable && e.blend[0].srcColorBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA);
    VERIFIE(e.blend[0].dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA && e.blend[0].colorBlendOp == VK_BLEND_OP_ADD);
    VERIFIE(e.blend[0].dstAlphaBlendFactor == VK_BLEND_FACTOR_ONE && e.blend[0].alphaBlendOp == VK_BLEND_OP_MAX);
    VERIFIE(e.blend[0].colorWriteMask == (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_A_BIT));

    // Écriture Z coupée sans test Z ; stencil deux faces : avant EQUAL/REPLACE, arrière ALWAYS/INCR ; réf/masques
    r.depthControl = 0x4 | 1 | 1 << 7 | 2 << 8 | 2 << 14 | 7 << 20 | 6 << 26;
    r.stencilRefMask = 0x00FF0F05;     // écriture 0xFF, masque 0x0F, réf 5
    r.stencilRefMaskBF = 0x00010203;
    e = TraduireEtat(r, true);
    VERIFIE(!e.profondeur.depthTestEnable && !e.profondeur.depthWriteEnable && e.profondeur.stencilTestEnable);
    VERIFIE(e.profondeur.front.compareOp == VK_COMPARE_OP_EQUAL && e.profondeur.front.passOp == VK_STENCIL_OP_REPLACE);
    VERIFIE(e.profondeur.front.reference == 5 && e.profondeur.front.compareMask == 0x0F && e.profondeur.front.writeMask == 0xFF);
    VERIFIE(e.profondeur.back.compareOp == VK_COMPARE_OP_ALWAYS && e.profondeur.back.passOp == VK_STENCIL_OP_INCREMENT_AND_WRAP);
    VERIFIE(e.profondeur.back.reference == 3 && e.profondeur.back.compareMask == 2 && e.profondeur.back.writeMask == 1);
    r.depthControl &= ~(1u << 7);      // TWOSIDEDSTENCILMODE coupé : arrière = avant
    VERIFIE(TraduireEtat(r, true).profondeur.back.compareOp == VK_COMPARE_OP_EQUAL);

    // Culling des faces arrière, avant = sens horaire ; fil de fer ; test alpha GREATER + alpha vers couverture
    r.modeControl = 2 | 4 | 1 << 3 | 1 << 5;
    r.colorControl = 4 | 1 << 3 | 1 << 4;
    e = TraduireEtat(r, true);
    VERIFIE(e.cull == VK_CULL_MODE_BACK_BIT && e.face == VK_FRONT_FACE_CLOCKWISE && e.polygone == VK_POLYGON_MODE_LINE);
    VERIFIE(e.testAlpha && e.fonctionAlpha == VK_COMPARE_OP_GREATER && e.alphaVersCouverture);
    r.modeControl = 1;
    VERIFIE(TraduireEtat(r, true).cull == VK_CULL_MODE_FRONT_BIT && TraduireEtat(r, true).face == VK_FRONT_FACE_COUNTER_CLOCKWISE);

    if (g_fail) { fprintf(stderr, "%d échec(s)\n", g_fail); return 1; }
    printf("unit_natif_etats : ok\n");
    return 0;
}
