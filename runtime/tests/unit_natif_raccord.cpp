// Test du raccord natif (natif/raccord.cpp) sur GPU : pour chaque paire de conteneurs <nom>.xvu / <nom>.xpu du jeu,
// empreinte à la création, module SPIR-V précompilé, entrée de sommets d'une déclaration (POSITION, TEXCOORD, COLOR),
// pipeline (avec et sans test alpha), constantes, textures et draw UP dans une image hors écran.
// Usage : wakfu_unit_natif_raccord <conteneurs> <dossier SPIR-V> ; WAKFU_VK_VALIDATION=1 pour la couche de validation.
#include <dirent.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>
#include "gpu/vk.h"
#include "guest.h"
#include "natif/interfaces.h"
#include "natif/tampons.h"
#include "platform.h"

using namespace wk::vk;
static Context g_c;
static Image g_tex;
static VkCommandBuffer g_envoi = VK_NULL_HANDLE;
// Fournis par device.cpp et textures.cpp dans le jeu : pas de resolve natif ici, toute texture lue est g_tex
namespace wk::natif { vk::Context* Contexte() { return &g_c; } VkCommandBuffer CmdEnvoi() { return g_envoi; }
VkImageView VueResolue(uint32_t) { return VK_NULL_HANDLE; } Poste Basculer(Poste p) { return p; }
VkImageView VueTexture(const uint32_t[6]) { return g_tex.view; }
VkImageView VueVide() { return g_tex.view; } void TexturesLot(uint32_t) {}
uint32_t VersionTextures() { return 0; } uint32_t EpoqueTextures() { return 0; }
[[noreturn]] void RenduImpossible(const std::string& c) { printf("ÉCHEC rendu impossible : %s\n", c.c_str()); exit(1); } }

static bool Charger(const std::string& f, uint32_t at) {
    std::ifstream in(f, std::ios::binary);
    std::vector<char> b((std::istreambuf_iterator<char>(in)), {});
    if (b.empty()) return false;
    memcpy(wk::Host(at), b.data(), b.size());
    return true;
}

int main(int argc, char** argv) {
    if (argc != 3) {   // données du jeu (conteneurs de shaders, SPIR-V) : fournies par la batterie, absentes en lancement seul
        printf("unit_natif_raccord : IGNORÉ (usage : %s <conteneurs> <dossier SPIR-V>)\n", argv[0]);
        return 0;
    }
    setenv("WAKFU_NATIF_SPIRV", argv[2], 1);
    wk::g_base = static_cast<uint8_t*>(wk::platform::ReserveGuestMemory(0x100000000ull));
    std::string err;
    if (!wk::g_base || !g_c.Init(&err)) { printf("ÉCHEC init : %s\n", err.c_str()); return 1; }
    printf("GPU : %s (validation %s, fonctions des shaders natifs %s)\n", g_c.deviceName.c_str(), g_c.validation ? "oui" : "non",
           g_c.natifShaders ? "oui" : "non");
    if (!::natif::BuffersInit(&g_c)) { printf("ÉCHEC tampons\n"); return 1; }
    Image cible;
    if (!g_c.CreateImage(64, 64, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &cible) ||
        !g_c.CreateImage(4, 4, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT, &g_tex)) { printf("ÉCHEC images\n"); return 1; }

    // Déclaration : POSITION0 FLOAT3 @0, TEXCOORD0 FLOAT2 @12, COLOR0 D3DCOLOR @20, fin (flux 0xFF)
    const uint32_t decl = 0x10100000;
    const struct { uint16_t off; uint32_t type; uint8_t usage; } el[3] = {{0, 0x2A23B9, 0}, {12, 0x2C23A5, 5}, {20, 0x182886, 10}};
    wk::St32(decl + 0x18, 3);
    for (int i = 0; i < 4; i++) {
        const uint32_t a = decl + 0x34 + 12 * i;
        wk::St16(a, i < 3 ? 0 : 0xFF);
        wk::St16(a + 2, i < 3 ? el[i].off : 0);
        wk::St32(a + 4, i < 3 ? el[i].type : 0xFFFFFFFF);
        wk::St8(a + 9, i < 3 ? el[i].usage : 0);
    }
    std::set<std::string> noms;
    if (DIR* dir = opendir(argv[1])) {
        while (dirent* de = readdir(dir)) {
            std::string n = de->d_name;
            if (n.size() > 4 && n.compare(n.size() - 4, 4, ".xvu") == 0) noms.insert(n.substr(0, n.size() - 4));
        }
        closedir(dir);
    }
    uint32_t paires = 0, rendus = 0, echecs = 0;
    for (const std::string& nom : noms) {
        const std::string base = std::string(argv[1]) + "/" + nom;
        if (!Charger(base + ".xvu", 0x10000000) || !Charger(base + ".xpu", 0x10080000)) continue;
        paires++;
        wk::natif::ShaderCree(0x20000000, 0x10000000);
        wk::natif::ShaderCree(0x20000010, 0x10080000);
        if (getenv("XENOSRECOMP")) {   // traduction au vol (SPIR-V absents) : refus (cause 2) jusqu'à la création du .spv
            wk::natif::Instantane d0;
            d0.vs = 0x20000000; d0.ps = 0x20000010;
            wk::natif::ShadersNatifs s0;
            for (int i = 0; i < 600 && !wk::natif::ObtenirShaders(d0, &s0) && s0.echec == 2; i++) usleep(100000);
        }
        for (uint32_t alpha = 0; alpha < 2; alpha++) {
            wk::natif::Instantane d;
            d.type = 2; d.prim = 4; d.nombre = d.sommetsUP = 3; d.pasUP = 24;
            d.vs = 0x20000000; d.ps = 0x20000010; d.declaration = decl;
            d.regs.colorControl = alpha << 3;
            d.textures[0] = 1;
            d.viewport[2] = d.viewport[3] = 64; d.viewport[5] = 1;
            d.constantes[0] = std::make_shared<std::array<float, 1024>>();
            d.constantes[1] = std::make_shared<std::array<float, 1024>>();
            d.donneesUP.assign(72, 0);
            wk::natif::ShadersNatifs sh;
            wk::natif::SommetsNatifs so;
            VkPipeline p = VK_NULL_HANDLE;
            g_c.Submit([&](VkCommandBuffer c) {
                g_envoi = c;   // aucune copie attendue (draw UP : anneau)
                wk::natif::NouveauLot(0, paires * 2 + alpha);
                if (!wk::natif::ObtenirShaders(d, &sh)) { printf("%s : shaders refusés (cause %u)\n", nom.c_str(), sh.echec); return; }
                if (!wk::natif::ObtenirSommets(d, &so)) { printf("%s : sommets refusés (cause %u)\n", nom.c_str(), so.echec); return; }
                VkPipelineShaderStageCreateInfo st[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
                st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = sh.vs; st[0].pName = "main";
                st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = sh.ps; st[1].pName = "main";
                st[0].pSpecializationInfo = st[1].pSpecializationInfo = sh.specialisation;
                VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
                ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
                vp.viewportCount = vp.scissorCount = 1;
                VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
                rs.lineWidth = 1;
                VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
                ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
                VkPipelineColorBlendAttachmentState ba{};
                ba.colorWriteMask = 0xF;
                VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
                cb.attachmentCount = 1;
                cb.pAttachments = &ba;
                const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
                VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
                dy.dynamicStateCount = 2;
                dy.pDynamicStates = dyn;
                const VkFormat fc = cible.format;
                VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
                ri.colorAttachmentCount = 1;
                ri.pColorAttachmentFormats = &fc;
                VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
                pci.pNext = &ri;
                pci.stageCount = 2; pci.pStages = st;
                pci.pVertexInputState = so.entree; pci.pInputAssemblyState = &ia; pci.pViewportState = &vp;
                pci.pRasterizationState = &rs; pci.pMultisampleState = &ms; pci.pColorBlendState = &cb; pci.pDynamicState = &dy;
                pci.layout = sh.disposition;
                if (vkCreateGraphicsPipelines(g_c.device, VK_NULL_HANDLE, 1, &pci, nullptr, &p) != VK_SUCCESS) {
                    printf("%s : pipeline refusé\n", nom.c_str());
                    return;
                }
                Context::Transition(c, cible.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
                Context::Transition(c, g_tex.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
                VkRenderingAttachmentInfo at{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                at.imageView = cible.view;
                at.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                at.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                VkRenderingInfo rinfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
                rinfo.renderArea = {{0, 0}, {64, 64}};
                rinfo.layerCount = 1;
                rinfo.colorAttachmentCount = 1;
                rinfo.pColorAttachments = &at;
                vkCmdBeginRendering(c, &rinfo);
                vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
                const VkViewport v{0, 0, 64, 64, 0, 1};
                const VkRect2D r{{0, 0}, {64, 64}};
                vkCmdSetViewport(c, 0, 1, &v);
                vkCmdSetScissor(c, 0, 1, &r);
                if (sh.lier(c, sh.disposition, d) == 2) printf("%s : liaison impossible\n", nom.c_str());
                else { so.dessiner(c, d); rendus++; }
                vkCmdEndRendering(c);
            });
            if (p) vkDestroyPipeline(g_c.device, p, nullptr);   // Submit attend la fin du GPU
        }
    }
    echecs = paires * 2 - rendus;
    printf("%u paires, %u draws rendus, %u échecs : %s\n", paires, rendus, echecs, paires && !echecs ? "OK" : "ÉCHEC");
    return paires && !echecs ? 0 : 1;
}
