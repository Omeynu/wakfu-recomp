// B3 : raccord du cœur natif (device.cpp) aux modules : shaders précompilés XenosRecomp -> SPIR-V (dossier
// WAKFU_NATIF_SPIRV, fichiers <empreinte XXH3 du conteneur>.spv, cf. packaging/shaders/precompile-shaders.sh),
// interface de docs/natif-b3-shaders.md §3 (constantes par adresse de tampon en push constants, tableaux de textures et
// d'échantillonneurs, test alpha par constante de spécialisation) ; sommets et indices par natif/sommets et
// natif/tampons ; textures : résultats des resolves natifs (VueResolue), sinon cache natif (natif/textures, N1), sinon texture vide. Fil natif, sauf ShaderCree.
#define XXH_INLINE_ALL
#include <xxhash.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "config.h"
#include "gpu/vk.h"
#include "guest.h"
#include "platform.h"
#include "natif/device.h"
#include "natif/interfaces.h"
#include "natif/sommets.h"
#include "natif/tampons.h"
#include "natif/textures.h"

namespace wk::natif {
namespace {
using namespace wk::vk;

constexpr uint32_t kEchantillonneurs = 16;      // registres d'échantillonneur lus par les shaders (0 à 8 dans ce jeu)
constexpr VkDeviceSize kAnneau = 32ull << 20;   // constantes par lot (≈ 8,5 Ko par draw au pire)
constexpr uint32_t kJeux = 4096;                // jeux de descripteurs par lot (2 par changement de textures)

// Empreintes des shaders créés (fil du jeu -> fil natif)
std::mutex g_mu;
std::unordered_map<uint32_t, uint64_t> g_empreintes;
std::atomic<uint32_t> g_traduits{0};   // SPIR-V créés pendant la partie (Module réessaie alors les absents)

struct Tampon { VkBuffer buf = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; uint8_t* map = nullptr; VkDeviceAddress adr = 0; };
struct Entree {   // entrée de sommets d'une déclaration et de ses pas
    ::natif::VertexLayout l;
    std::vector<VkVertexInputBindingDescription> liaisons;
    std::vector<VkVertexInputAttributeDescription> attributs;
    VkPipelineVertexInputStateCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    uint32_t liaisonZero = UINT32_MAX;   // flux sans données (attributs attendus par le VS, absents de la déclaration)
};
// Draw en cours (un seul fil natif) : rempli par ObtenirSommets, lu par Dessiner
struct Courant {
    const Entree* e = nullptr;
    ::natif::Buffer vb[16];
    ::natif::Buffer ib;
    VkIndexType typeIndices = VK_INDEX_TYPE_UINT16;
    uint32_t indicesQuads = 0;   // liste de quads : nombre d'indices de triangles générés (c.ib, 32 bits)
};

struct Declaration { std::vector<uint8_t> octets; ::natif::VertexLayout l; };
struct Etat {
    Context* vk = nullptr;
    bool essaye = false, pret = false;
    std::string dossier;
    VkDescriptorSetLayout ensembles[4]{};
    VkPipelineLayout disposition = VK_NULL_HANDLE;
    std::unordered_map<uint64_t, VkShaderModule> modules;   // VK_NULL_HANDLE : absent ou invalide
    uint32_t traduits = 0;                                    // g_traduits lors du dernier ménage des absents
    Tampon anneaux[2], zero;
    VkDeviceSize pos[2]{};
    VkDescriptorPool pools[2]{};
    uint32_t slot = 0, image = UINT32_MAX;
    std::unordered_map<uint64_t, VkSampler> echantillonneurs;
    // derniers blocs écrits dans le lot (réutilisés tant que rien ne change)
    uint64_t serieConstantes[2]{};   // instantanés VS, PS déjà écrits dans le lot (adrVs, adrPs)
    VkDeviceAddress adrVs = 0, adrPs = 0, adrPartage = 0;
    uint8_t partage[288]{};
    VkImageView vues[kEchantillonneurs]{};
    VkSampler ech[kEchantillonneurs]{};
    VkDescriptorSet jeux[2]{};
    // liés dans le tampon de commandes du lot (non réémis s'ils ne changent pas ; remis à zéro à chaque lot)
    VkDescriptorSet jeuxLies[2]{};
    VkDeviceAddress pushLie[3]{};
    VkBuffer vbLie[16]{}, ibLie = VK_NULL_HANDLE;
    VkDeviceSize vbOffLie[16]{}, ibOffLie = 0;
    VkIndexType ibTypeLie = VK_INDEX_TYPE_MAX_ENUM;
    // vue et échantillonneur de chaque registre au draw précédent, valables dans le lot tant que constante de lecture,
    // époque des textures et version des resolves ne changent pas (ech nul : à recalculer)
    struct { uint32_t f[6]; bool texture, secours; VkImageView vue; VkSampler ech; } memo[kEchantillonneurs]{};
    uint32_t memoEpoque = 0, memoVersion = 0;
    std::unordered_map<uint64_t, std::unique_ptr<Entree>> entrees;
    std::unordered_map<uint32_t, Declaration> declarations;   // objet déclaration invité -> disposition
    Courant courant;
} e;

bool CreerTampon(VkDeviceSize taille, VkBufferUsageFlags usage, Tampon* t) {
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = taille;
    bci.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (vkCreateBuffer(e.vk->device, &bci, nullptr, &t->buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(e.vk->device, t->buf, &req);
    VkMemoryAllocateFlagsInfo fl{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    fl.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &fl;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = e.vk->FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    void* p = nullptr;
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(e.vk->device, &ai, nullptr, &t->mem) != VK_SUCCESS ||
        vkBindBufferMemory(e.vk->device, t->buf, t->mem, 0) != VK_SUCCESS ||
        vkMapMemory(e.vk->device, t->mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS)
        return false;
    t->map = static_cast<uint8_t*>(p);
    VkBufferDeviceAddressInfo bi{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    bi.buffer = t->buf;
    t->adr = vkGetBufferDeviceAddress(e.vk->device, &bi);
    return true;
}

// Disposition commune à tous les shaders : set 0 Texture2D[], sets 1/2 (3D, cube : inutilisés par le jeu) vides,
// set 3 SamplerState[] ; push constants = 3 adresses (VS, PS, partagées).
// WAKFU_NATIF_SPIRV, sinon <dossier de l'exécutable>/spirv (installation du lanceur), sinon spirv (dossier courant)
std::string DossierSpirv() {
    static const std::string d = [] {
        if (const char* v = getenv("WAKFU_NATIF_SPIRV"); v && *v) return std::string(v);
        const std::string x = platform::ExeDir();
        return x.empty() ? std::string("spirv") : x + "/spirv";
    }();
    return d;
}

bool Preparer() {
    if (e.essaye) return e.pret;
    e.essaye = true;
    e.vk = Contexte();
    e.dossier = DossierSpirv();
    std::error_code ec;
    if (!std::filesystem::is_directory(e.dossier, ec))
        RenduImpossible("shaders précompilés absents (dossier " + e.dossier + ") : réinstaller le jeu avec WakfuRecomp");
    if (!e.vk) return false;
    if (!e.vk->natifShaders)
        RenduImpossible("le GPU " + e.vk->deviceName + " n'a pas " + e.vk->natifManque + " (Vulkan 1.2, pilote à jour ?)");
    const VkShaderStageFlags etages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    for (int s = 0; s < 4; s++) {
        VkDescriptorSetLayoutBinding b{0, s == 0 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER,
                                       kEchantillonneurs, etages, nullptr};
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = (s == 0 || s == 3) ? 1 : 0;
        ci.pBindings = &b;
        if (vkCreateDescriptorSetLayout(e.vk->device, &ci, nullptr, &e.ensembles[s]) != VK_SUCCESS) return false;
    }
    const VkPushConstantRange pc{etages, 0, 24};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 4;
    pli.pSetLayouts = e.ensembles;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(e.vk->device, &pli, nullptr, &e.disposition) != VK_SUCCESS) return false;
    const VkDescriptorPoolSize tailles[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kJeux / 2 * kEchantillonneurs},
                                             {VK_DESCRIPTOR_TYPE_SAMPLER, kJeux / 2 * kEchantillonneurs}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = kJeux;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = tailles;
    for (int i = 0; i < 2; i++) {
        if (vkCreateDescriptorPool(e.vk->device, &dpi, nullptr, &e.pools[i]) != VK_SUCCESS) return false;
        if (!CreerTampon(kAnneau, 0, &e.anneaux[i])) return false;
    }
    if (!CreerTampon(16, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &e.zero)) return false;
    const float defaut[4] = {0, 0, 0, 1};   // élément absent de la déclaration : valeur D3D par défaut
    memcpy(e.zero.map, defaut, 16);
    fprintf(stderr, "[natif] shaders précompilés lus dans %s\n", e.dossier.c_str());
    return e.pret = true;
}

// Bloc écrit dans l'anneau du lot ; 0 si plein.
VkDeviceAddress Ecrire(const void* p, VkDeviceSize taille) {
    const VkDeviceSize at = (e.pos[e.slot] + 255) & ~VkDeviceSize(255);
    if (at + taille > kAnneau) return 0;
    memcpy(e.anneaux[e.slot].map + at, p, taille);
    e.pos[e.slot] = at + taille;
    return e.anneaux[e.slot].adr + at;
}

VkShaderModule Module(uint64_t h) {
    if (const uint32_t n = g_traduits.load(std::memory_order_acquire); n != e.traduits) {
        e.traduits = n;
        for (auto i = e.modules.begin(); i != e.modules.end();) i = i->second ? std::next(i) : e.modules.erase(i);
    }
    auto it = e.modules.find(h);
    if (it != e.modules.end()) return it->second;
    VkShaderModule m = VK_NULL_HANDLE;
    char nom[32];
    snprintf(nom, sizeof nom, "/%016llx.spv", (unsigned long long)h);
    if (FILE* f = fopen((e.dossier + nom).c_str(), "rb")) {
        std::vector<uint32_t> spv;
        uint32_t mot;
        while (fread(&mot, 4, 1, f) == 1) spv.push_back(mot);
        fclose(f);
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = spv.size() * 4;
        ci.pCode = spv.data();
        if (spv.size() < 5 || spv[0] != 0x07230203u || vkCreateShaderModule(e.vk->device, &ci, nullptr, &m) != VK_SUCCESS)
            m = VK_NULL_HANDLE;
    }
    return e.modules[h] = m;
}

// Constante de lecture de texture -> échantillonneur (adressage mot 0 bits 10-15, filtres mot 3 bits 19-22)
VkSampler Echantillonneur(const uint32_t* f) {
    const uint32_t cx = (f[0] >> 10) & 7, cy = (f[0] >> 13) & 7, mag = (f[3] >> 19) & 3, min = (f[3] >> 21) & 3,
                   mip = (f[3] >> 23) & 3;
    const uint64_t k = cx | cy << 3 | mag << 6 | min << 8 | mip << 10;
    VkSampler& s = e.echantillonneurs[k];
    if (s) return s;
    auto mode = [](uint32_t c) {   // Xenos : WRAP, MIRROR, CLAMP_LAST_TEXEL, MIRROR_ONCE_LAST_TEXEL, CLAMP_HALF_WAY,
        switch (c) {               // MIRROR_ONCE_HALF_WAY, CLAMP_BORDER, MIRROR_ONCE_BORDER
            case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
            case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            case 6: case 7: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;   // shortcut: miroir une fois = bord
        }
    };
    VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sc.magFilter = mag == 0 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    sc.minFilter = min == 0 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    sc.addressModeU = mode(cx);
    sc.addressModeV = mode(cy);
    sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sc.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    // mips (N1) : POINT, LINEAR, BASEMAP (niveau 0 seul) ; shortcut: lod_bias et anisotropie du jeu ignorés
    sc.mipmapMode = mip == 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sc.maxLod = mip == 2 ? 0.0f : VK_LOD_CLAMP_NONE;
    // option [video] filtrage_anisotrope (échantillonneurs filtrés seulement), plafonnée par le GPU
    if (config::g.anisotropy > 1 && e.vk->maxAnisotropy >= 1 && min != 0) {
        sc.anisotropyEnable = VK_TRUE;
        sc.maxAnisotropy = std::min(float(config::g.anisotropy), e.vk->maxAnisotropy);
    }
    if (vkCreateSampler(e.vk->device, &sc, nullptr, &s) != VK_SUCCESS) s = VK_NULL_HANDLE;
    return s;
}

int Lier(VkCommandBuffer cmd, VkPipelineLayout disposition, const Instantane& d) {
    Basculer(kConstantes);
    // Constantes float VS c0-c255 et PS c0-c255 (4 096 o chacune), partagées tant que l'instantané ne change pas
    if (d.serieConstantes[0] != e.serieConstantes[0] || !e.adrVs) {
        e.adrVs = Ecrire(d.constantes[0]->data(), 4096);
        e.serieConstantes[0] = d.serieConstantes[0];
    }
    if (d.serieConstantes[1] != e.serieConstantes[1] || !e.adrPs) {
        e.adrPs = Ecrire(d.constantes[1]->data(), 4096);
        e.serieConstantes[1] = d.serieConstantes[1];
    }
    // Constantes partagées (docs/natif-b3-shaders.md §3) : échantillonneur s -> texture s, échantillonneur s
    uint8_t partage[288] = {};
    uint32_t* u = reinterpret_cast<uint32_t*>(partage);
    for (uint32_t s = 0; s < kEchantillonneurs; s++) u[s] = u[48 + s] = s;
    // g_HalfPixelOffset (+264) laissé à 0 : le jeu pose PA_SU_VTX_CNTL = 4 et l'émulation (référence) n'applique aucun
    // décalage ; celui d'Unleashed (1/l, -1/h) décalait le natif d'un demi-pixel vers la droite et le bas
    memcpy(partage + 272, &d.refAlpha, 4);
    if (!e.adrPartage || memcmp(partage, e.partage, sizeof partage)) {
        e.adrPartage = Ecrire(partage, sizeof partage);
        memcpy(e.partage, partage, sizeof partage);
    }
    if (!e.adrVs || !e.adrPs || !e.adrPartage) { e.adrVs = e.adrPartage = 0; return 2; }   // anneau plein
    const VkDeviceAddress push[3] = {e.adrVs, e.adrPs, e.adrPartage};
    if (memcmp(push, e.pushLie, sizeof push)) {
        vkCmdPushConstants(cmd, disposition, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, push);
        memcpy(e.pushLie, push, sizeof push);
    }
    // Textures : celles posées par SetTexture (dev+0x31F8) ; résultat du resolve natif si la mémoire en vient (shortcut:
    // une texture réécrite par le CPU après un resolve reste servie par l'image native, à suivre si cela arrive), sinon
    // cache natif, sinon texture vide (comptée « de secours »)
    Basculer(kTextures);
    bool secours = false;
    const VkImageView vide = VueVide();
    VkImageView vues[kEchantillonneurs];
    VkSampler ech[kEchantillonneurs];
    if (const uint32_t ep = EpoqueTextures(), ve = VersionTextures(); ep != e.memoEpoque || ve != e.memoVersion) {
        for (auto& m : e.memo) m.ech = VK_NULL_HANDLE;
        e.memoEpoque = ep;
        e.memoVersion = ve;
    }
    for (uint32_t s = 0; s < kEchantillonneurs; s++) {
        const uint32_t* f = d.fetch + 6 * s;
        auto& m = e.memo[s];
        if (m.ech && m.texture == (d.textures[s] != 0) && !memcmp(m.f, f, sizeof m.f)) {
            vues[s] = m.vue;
            ech[s] = m.ech;
            secours |= m.secours;
            continue;
        }
        bool sec = false;
        vues[s] = vide;
        if (d.textures[s]) {
            vues[s] = VueResolue(f[1]);
            if (!vues[s]) vues[s] = VueTexture(f);
            if (!vues[s]) {
                vues[s] = vide;
                sec = true;
            }
        }
        ech[s] = Echantillonneur(f);
        if (!vues[s] || !ech[s]) return 2;
        secours |= sec;
        memcpy(m.f, f, sizeof m.f);
        m.texture = d.textures[s] != 0;
        m.secours = sec;
        m.vue = vues[s];
        m.ech = ech[s];
    }
    Basculer(kDescripteurs);
    if (!e.jeux[0] || memcmp(vues, e.vues, sizeof vues) || memcmp(ech, e.ech, sizeof ech)) {
        const VkDescriptorSetLayout l[2] = {e.ensembles[0], e.ensembles[3]};
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = e.pools[e.slot];
        ai.descriptorSetCount = 2;
        ai.pSetLayouts = l;
        if (vkAllocateDescriptorSets(e.vk->device, &ai, e.jeux) != VK_SUCCESS) { e.jeux[0] = VK_NULL_HANDLE; return 2; }
        VkDescriptorImageInfo ii[2][kEchantillonneurs];
        for (uint32_t s = 0; s < kEchantillonneurs; s++) {
            ii[0][s] = {VK_NULL_HANDLE, vues[s], VK_IMAGE_LAYOUT_GENERAL};
            ii[1][s] = {ech[s], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        }
        VkWriteDescriptorSet w[2]{};
        for (int i = 0; i < 2; i++) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = e.jeux[i];
            w[i].descriptorCount = kEchantillonneurs;
            w[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            w[i].pImageInfo = ii[i];
        }
        vkUpdateDescriptorSets(e.vk->device, 2, w, 0, nullptr);
        memcpy(e.vues, vues, sizeof vues);
        memcpy(e.ech, ech, sizeof ech);
    }
    if (e.jeux[0] != e.jeuxLies[0]) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, disposition, 0, 1, &e.jeux[0], 0, nullptr);
    if (e.jeux[1] != e.jeuxLies[1]) vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, disposition, 3, 1, &e.jeux[1], 0, nullptr);
    e.jeuxLies[0] = e.jeux[0];
    e.jeuxLies[1] = e.jeux[1];
    return secours ? 1 : 0;
}

// Entrées fixes des VS précompilés (table d'Unleashed, docs/natif-b3-shaders.md §3) ; -1 : non lue
int Emplacement(uint32_t usage, uint32_t index) {
    switch (usage) {
        case ::natif::kPosition: return index == 0 ? 0 : -1;
        case ::natif::kNormal: return index == 0 ? 1 : -1;
        case ::natif::kTangent: return index == 0 ? 2 : -1;
        case ::natif::kBinormal: return index == 0 ? 3 : -1;
        case ::natif::kTexcoord: return index < 4 ? 4 + int(index) : index < 8 ? 8 + int(index) : -1;
        case ::natif::kColor: return index == 0 ? 8 : index == 1 ? 11 : -1;
        case ::natif::kBlendIndices: return index == 0 ? 9 : -1;
        case ::natif::kBlendWeight: return index == 0 ? 10 : -1;
        default: return -1;
    }
}

const Entree* EntreePour(const ::natif::VertexLayout& l, const uint32_t pas[16]) {
    uint64_t k = l.key;
    for (int s = 0; s < 16; s++) k = (k ^ pas[s]) * 1099511628211ull;
    std::unique_ptr<Entree>& en = e.entrees[k];
    if (en) return en.get();
    en = std::make_unique<Entree>();
    en->l = l;
    std::vector<VkVertexInputAttributeDescription> brut;
    ::natif::Describe(en->l, pas, &en->liaisons, &brut);
    uint32_t pris = 0;
    for (const VkVertexInputAttributeDescription& a : brut) {
        const ::natif::Attribute& at = en->l.attrs[a.location];
        const int loc = Emplacement(at.usage, at.usageIndex);
        if (loc < 0 || (pris >> loc) & 1 || a.format == VK_FORMAT_UNDEFINED) continue;
        pris |= 1u << loc;
        en->attributs.push_back({uint32_t(loc), a.binding, a.format, a.offset});
    }
    if (pris != 0xFFFF) {   // emplacements que le VS peut lire sans donnée : (0, 0, 0, 1) d'un flux à pas nul
        for (uint32_t s = 16; s-- > 0;)
            if (!(en->l.streamMask >> s & 1)) { en->liaisonZero = s; break; }
        en->liaisons.push_back({en->liaisonZero, 0, VK_VERTEX_INPUT_RATE_VERTEX});
        for (uint32_t loc = 0; loc < 16; loc++)
            if (!(pris >> loc & 1)) en->attributs.push_back({loc, en->liaisonZero, VK_FORMAT_R32G32B32A32_SFLOAT, 0});
    }
    en->info.vertexBindingDescriptionCount = uint32_t(en->liaisons.size());
    en->info.pVertexBindingDescriptions = en->liaisons.data();
    en->info.vertexAttributeDescriptionCount = uint32_t(en->attributs.size());
    en->info.pVertexAttributeDescriptions = en->attributs.data();
    return en.get();
}

void Dessiner(VkCommandBuffer cmd, const Instantane& d) {
    const Courant& c = e.courant;
    for (const VkVertexInputBindingDescription& b : c.e->liaisons) {
        const bool zero = b.binding == c.e->liaisonZero;
        const VkBuffer buf = zero ? e.zero.buf : c.vb[b.binding].buffer;
        const VkDeviceSize off = zero ? 0 : c.vb[b.binding].offset;
        if (buf == e.vbLie[b.binding] && off == e.vbOffLie[b.binding]) continue;
        vkCmdBindVertexBuffers(cmd, b.binding, 1, &buf, &off);
        e.vbLie[b.binding] = buf;
        e.vbOffLie[b.binding] = off;
    }
    auto lierIndices = [&](VkCommandBuffer cmd) {
        if (c.ib.buffer == e.ibLie && c.ib.offset == e.ibOffLie && c.typeIndices == e.ibTypeLie) return;
        vkCmdBindIndexBuffer(cmd, c.ib.buffer, c.ib.offset, c.typeIndices);
        e.ibLie = c.ib.buffer;
        e.ibOffLie = c.ib.offset;
        e.ibTypeLie = c.typeIndices;
    };
    if (d.prim == 13) {   // indices générés par ObtenirSommets (début et base déjà appliqués)
        if (!c.indicesQuads) return;
        lierIndices(cmd);
        vkCmdDrawIndexed(cmd, c.indicesQuads, 1, 0, 0, 0);
        return;
    }
    switch (d.type) {
        case 0: vkCmdDraw(cmd, d.nombre, 1, d.debut, 0); break;
        case 1:
            lierIndices(cmd);
            vkCmdDrawIndexed(cmd, d.nombre, 1, d.debut, d.baseSommet, 0);
            break;
        case 2: vkCmdDraw(cmd, d.nombre, 1, 0, 0); break;
        default:
            lierIndices(cmd);
            vkCmdDrawIndexed(cmd, d.nombre, 1, 0, 0, 0);
    }
}

// Conteneur -> SPIR-V comme packaging/shaders/precompile-shaders.sh (XenosRecomp puis DXC, mêmes options), outils
// désignés par les mêmes variables ; .spv écrit sous un nom temporaire puis renommé (Module ne lit jamais un fichier partiel).
// shortcut: pas de délai maximal (le script en met un) ; un outil bloqué bloque seulement les traductions suivantes.
void Traduire(const std::string& conteneur, const std::string& spv, bool vs) {
    const char *xr = getenv("XENOSRECOMP"), *inc = getenv("XENOS_INCLUDE"), *dxc = getenv("DXC");
    if (!xr || !inc || !dxc) return;
    const std::string hlsl = spv + ".hlsl", tmp = spv + ".tmp";
    int rc = ::wk::platform::RunTool({xr, conteneur, hlsl, inc}, getenv("DXC_LIB"));   // XenosRecomp est lié à libdxcompiler
    const bool xrOk = rc == 0;
    if (xrOk) {
        std::vector<std::string> a = {dxc, vs ? "-Tvs_6_0" : "-Tps_6_0", "-HV", "2021", "-all-resources-bound", "-spirv",
                                      "-fvk-use-dx-layout", "-Qstrip_debug", "-E", "main", "-Fo", tmp, hlsl};
        if (vs) a.insert(a.begin() + 7, "-fvk-invert-y");
        rc = ::wk::platform::RunTool(a, getenv("DXC_LIB"));
    }
    std::error_code ec;
    std::filesystem::remove(hlsl, ec);
    if (rc == 0) std::filesystem::rename(tmp, spv, ec);
    if (rc != 0 || ec) {
        std::filesystem::remove(tmp, ec);
        fprintf(stderr, "[natif] traduction de %s : échec %s (%d)\n", conteneur.c_str(), xrOk ? "dxc" : "xenosrecomp", rc);
        return;
    }
    fprintf(stderr, "[natif] traduction de %s : %s\n", conteneur.c_str(), spv.c_str());
    g_traduits.fetch_add(1, std::memory_order_release);
}

// Un seul fil de traduction (un outil à la fois : ne pas concurrencer le jeu), créé à la première demande et détaché.
// Son état est alloué et jamais détruit : à la sortie le fil peut encore attendre ou traduire sans toucher un statique détruit.
struct Tache { std::string conteneur, spv; bool vs; };
struct FileTraduction { std::mutex m; std::condition_variable cv; std::deque<Tache> q; };

FileTraduction& LaFile() {
    static FileTraduction* f = [] {
        auto* n = new FileTraduction;
        std::thread([n] {
            for (;;) {
                Tache t;
                {
                    std::unique_lock<std::mutex> l(n->m);
                    n->cv.wait(l, [n] { return !n->q.empty(); });
                    t = std::move(n->q.front());
                    n->q.pop_front();
                }
                try { Traduire(t.conteneur, t.spv, t.vs); } catch (...) {}   // une erreur perd ce shader, pas le fil
            }
        }).detach();
        return n;
    }();   // si le fil ne peut être créé, l'exception remonte et l'initialisation sera retentée à la demande suivante
    return *f;
}

void DemanderTraduction(const std::string& conteneur, const std::string& spv, bool vs) {
    try {
        FileTraduction& f = LaFile();
        { std::lock_guard<std::mutex> l(f.m); f.q.push_back({conteneur, spv, vs}); }
        f.cv.notify_one();
    } catch (const std::exception& x) {   // rien ne doit sortir dans le fil du jeu : le lanceur traduira au lancement suivant
        fprintf(stderr, "[natif] traduction de %s non demandée : %s\n", conteneur.c_str(), x.what());
    }
}

}  // namespace

void ShaderCree(uint32_t objet, uint32_t conteneur) {
    if (!objet || !conteneur) return;
    const uint32_t entete = Ld32(conteneur);
    if ((entete & 0xFFFFFF00u) != 0x102A1100u) return;
    const uint64_t taille = uint64_t(Ld32(conteneur + 4)) + Ld32(conteneur + 8);
    if (taille > (1u << 20)) return;
    const uint64_t h = XXH3_64bits(Host(conteneur), size_t(taille));
    // Shader sans SPIR-V (ex. compilé depuis du HLSL par le D3DX du XEX, sub_823CF4B0) : conteneur déposé dans
    // <dossier>/conteneurs, puis traduit sur un fil de fond si le lanceur a passé les outils (sinon par le lanceur
    // au lancement suivant). Un conteneur déjà présent a déjà été soumis aux outils (installation ou partie précédente).
    {
        const std::string d = DossierSpirv();
        char nom[64];
        snprintf(nom, sizeof nom, "/%016llx.spv", (unsigned long long)h);
        const std::string spv = d + nom, rep = d + "/conteneurs";
        snprintf(nom, sizeof nom, "/%016llx.%s", (unsigned long long)h, (entete & 1) ? "xvu" : "xpu");
        std::error_code ec;
        if (!std::filesystem::exists(spv, ec) && !std::filesystem::exists(rep + nom, ec)) {
            std::filesystem::create_directory(rep, ec);
            if (FILE* g = fopen((rep + nom).c_str(), "wb")) {
                const bool ok = fwrite(Host(conteneur), 1, size_t(taille), g) == size_t(taille);
                if (fclose(g) != 0 || !ok) remove((rep + nom).c_str());   // pas de conteneur tronqué
                else {
                    fprintf(stderr, "[natif] shader sans SPIR-V %016llx : conteneur déposé dans %s\n", (unsigned long long)h, rep.c_str());
                    DemanderTraduction(rep + nom, spv, (entete & 1) != 0);
                }
            }
        }
    }
    std::lock_guard<std::mutex> l(g_mu);
    g_empreintes[objet] = h;
}

void ShaderVu(uint32_t, bool) {}

void NouveauLot(uint32_t slot, uint32_t image) {
    if (image != e.image) ::natif::BuffersBeginFrame(image);   // une fois par image (un lot de relecture peut suivre)
    TexturesLot(image);
    e.image = image;
    e.slot = slot & 1;
    e.pos[e.slot] = 0;
    e.adrVs = e.adrPs = e.adrPartage = 0;
    e.jeux[0] = e.jeux[1] = VK_NULL_HANDLE;
    e.jeuxLies[0] = e.jeuxLies[1] = VK_NULL_HANDLE;
    memset(e.pushLie, 0, sizeof e.pushLie);
    memset(e.vbLie, 0, sizeof e.vbLie);
    e.ibLie = VK_NULL_HANDLE;
    e.ibTypeLie = VK_INDEX_TYPE_MAX_ENUM;
    for (auto& m : e.memo) m.ech = VK_NULL_HANDLE;
    if (e.pools[e.slot]) vkResetDescriptorPool(e.vk->device, e.pools[e.slot], 0);
}

bool ObtenirShaders(const Instantane& d, ShadersNatifs* out) {
    if (!Preparer()) {
        static bool dit = false;
        if (!dit) fprintf(stderr, "[natif] rendu impossible : shaders natifs indisponibles (GPU ou dossier SPIR-V)\n");
        dit = true;
        out->echec = 3;
        return false;
    }
    uint64_t hv = 0, hp = 0;
    {
        std::lock_guard<std::mutex> l(g_mu);
        auto iv = g_empreintes.find(d.vs), ip = g_empreintes.find(d.ps);
        if (iv == g_empreintes.end() || ip == g_empreintes.end()) { out->echec = 1; return false; }
        hv = iv->second;
        hp = ip->second;
    }
    out->vs = Module(hv);
    out->ps = Module(hp);
    if (!out->vs || !out->ps) { out->echec = 2; return false; }
    // bit 1 : test alpha (RB_COLORCONTROL bit 3) ; fonction toujours « ≥ » (cf. docs/natif-b3-shaders.md)
    static const uint32_t kValeurs[2] = {0, 2};
    static const VkSpecializationMapEntry kEntree{0, 0, 4};
    static const VkSpecializationInfo kSpec[2] = {{1, &kEntree, 4, &kValeurs[0]}, {1, &kEntree, 4, &kValeurs[1]}};
    const uint32_t alpha = (d.regs.colorControl >> 3) & 1;
    out->specialisation = &kSpec[alpha];
    out->disposition = e.disposition;
    out->cle = (hv * 0x9E3779B97F4A7C15ull ^ hp) + alpha;
    out->lier = Lier;
    return true;
}

bool ObtenirSommets(const Instantane& d, SommetsNatifs* out) {
    if (!d.declaration) { out->echec = 1; return false; }
    // Déclaration lue une fois par objet, revalidée par ses octets (objet libéré puis adresse réutilisée)
    const uint32_t nb = std::min<uint32_t>(Ld32(d.declaration + 0x18), 64);
    const uint8_t* el = Host(d.declaration + 0x34);
    Declaration& dc = e.declarations[d.declaration];
    if (dc.octets.size() != nb * 12u || memcmp(dc.octets.data(), el, nb * 12u)) {
        dc.octets.assign(el, el + nb * 12u);
        dc.l = ::natif::FromElements(el, nb);
    }
    const ::natif::VertexLayout& l = dc.l;
    if (l.attrs.empty()) { out->echec = 1; return false; }
    const bool up = d.type >= 2;
    const float cible[2] = {float(out->cible.width), float(out->cible.height)};
    const float* pixels = (d.vte & 5) != 5 ? cible : nullptr;   // VTE sans échelle X/Y (comme l'émulation)
    uint32_t pas[16] = {};
    if (up) pas[0] = d.pasUP;
    else memcpy(pas, d.pas, sizeof pas);
    Courant& c = e.courant;
    c = Courant{};
    out->echec = 2;
    if (up) {   // sommets (et indices) recopiés au draw : convertis dans l'anneau de l'image
        const uint64_t nbS = d.type == 3 ? uint64_t(uint32_t(d.baseSommet)) + d.sommetsUP : d.sommetsUP;
        const uint64_t tS = nbS * d.pasUP, tI = d.donneesUP.size() - std::min<uint64_t>(tS, d.donneesUP.size());
        if (d.donneesUP.empty() || !d.pasUP || tS > d.donneesUP.size()) return false;
        uint8_t* p = nullptr;
        c.vb[0] = ::natif::RingUpload(nbS * ::natif::PcStride(l, 0, d.pasUP), &p);
        if (!p) return false;
        ::natif::ConvertVertices(l, 0, d.donneesUP.data(), d.pasUP, uint32_t(nbS), p);
        if (pixels) ::natif::PixelsVersNdc(l, 0, d.pasUP, uint32_t(nbS), cible[0], cible[1], p);
        if (d.type == 3) {
            const bool is32 = d.formatIndicesUP == 6;
            c.typeIndices = is32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
            c.ib = ::natif::RingUpload(tI, &p);
            if (!p || !tI) return false;
            ::natif::ConvertIndices(d.donneesUP.data() + tS, uint32_t(tI), is32, p);
        }
    } else {
        for (uint32_t s = 0; s < 16; s++) {
            if (!(l.streamMask >> s & 1)) continue;
            // décalage de SetStreamSource : adresse de la constante de lecture de sommets 95-s moins celle du VB
            const uint32_t vb = d.flux[s];
            if (!vb) return false;
            const int64_t off = int64_t(d.fetch[190 - 2 * s] & ~3u) - int64_t(Ld32(vb + 24) & ~3u);
            c.vb[s] = ::natif::BufferFor(CmdEnvoi(), vb, l, s, off > 0 ? uint32_t(off) : 0, d.pas[s], pixels);
            if (!c.vb[s].size) return false;
        }
        if (d.type == 1) {
            c.ib = ::natif::IndexBufferFor(CmdEnvoi(), d.indices, &c.typeIndices);
            if (!c.ib.size) return false;
        }
    }
    if (d.prim == 13) {   // quads -> triangles indexés (indices UP déjà gros-boutistes dans donneesUP)
        const uint32_t quads = d.nombre / 4;
        std::vector<uint32_t> src, tri(size_t(quads) * 6);
        if (d.type == 3) {
            const bool is32 = d.formatIndicesUP == 6;
            const uint64_t tS = (uint64_t(uint32_t(d.baseSommet)) + d.sommetsUP) * d.pasUP, tI = uint64_t(d.nombre) * (is32 ? 4 : 2);
            if (tS + tI > d.donneesUP.size()) return false;
            std::vector<uint8_t> t(tI);
            ::natif::ConvertIndices(d.donneesUP.data() + tS, uint32_t(tI), is32, t.data());
            src.resize(d.nombre);
            for (uint32_t i = 0; i < d.nombre; i++) {
                if (is32) memcpy(&src[i], &t[4 * i], 4);
                else { uint16_t v; memcpy(&v, &t[2 * i], 2); src[i] = v; }
            }
        }
        ::natif::QuadIndices(src.empty() ? nullptr : src.data(), d.type == 0 ? d.debut : 0, quads, tri.data());
        if (quads) {
            uint8_t* p = nullptr;
            c.ib = ::natif::RingUpload(tri.size() * 4, &p);
            if (!p) return false;
            memcpy(p, tri.data(), tri.size() * 4);
        }
        c.typeIndices = VK_INDEX_TYPE_UINT32;
        c.indicesQuads = uint32_t(tri.size());
    }
    c.e = EntreePour(l, pas);
    out->echec = 0;
    out->entree = &c.e->info;
    out->cle = c.e->l.key ^ uint64_t(c.e->liaisons.size()) << 56;
    for (const VkVertexInputBindingDescription& b : c.e->liaisons) out->cle = (out->cle ^ b.stride) * 1099511628211ull;
    out->dessiner = Dessiner;
    return true;
}

}  // namespace wk::natif
