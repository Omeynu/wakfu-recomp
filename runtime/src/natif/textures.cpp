// N1 : cache de textures du rendu natif (cf. textures.h). Fil natif, sauf TextureEcrite / TextureDetruite (fil du jeu).
#include "natif/textures.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "gpu/tiling.h"
#include "guest.h"
#include "natif/device.h"
#include "natif/interfaces.h"
#include "natif/tampons.h"

namespace wk::natif {
using namespace wk::vk;

// ====================== Mise en page invitée ======================
namespace {
constexpr uint32_t kPhys = 0xA0000000u, kPhysTaille = 0x20000000u;

struct Format { uint8_t bpe, bloc, conversion; VkFormat vk; };
// Formats Xenos gérés (numéros de xenos.h) ; 16 bits couleur : développés en RGBA8
bool FormatDe(uint32_t f, Format* o) {
    switch (f) {
        case 2: *o = {1, 1, 0, VK_FORMAT_R8_UNORM}; return true;                  // k_8 : (x, 0, 0, 1)
        case 10: *o = {2, 1, 0, VK_FORMAT_R8G8_UNORM}; return true;               // k_8_8
        case 6: *o = {4, 1, 0, VK_FORMAT_R8G8B8A8_UNORM}; return true;            // k_8_8_8_8 (x = octet 0)
        case 3: case 4: case 15: *o = {2, 1, 1, VK_FORMAT_R8G8B8A8_UNORM}; return true;   // 1_5_5_5, 5_6_5, 4_4_4_4
        case 18: *o = {8, 4, 0, VK_FORMAT_BC1_RGBA_UNORM_BLOCK}; return true;     // DXT1
        case 19: *o = {16, 4, 0, VK_FORMAT_BC2_UNORM_BLOCK}; return true;         // DXT2/3
        case 20: *o = {16, 4, 0, VK_FORMAT_BC3_UNORM_BLOCK}; return true;         // DXT4/5
        case 49: *o = {16, 4, 0, VK_FORMAT_BC5_UNORM_BLOCK}; return true;         // DXN
        case 22: case 23: *o = {4, 1, 2, VK_FORMAT_R32_SFLOAT}; return true;      // 24_8 (profondeur sur 24 bits hauts)
        case 30: *o = {2, 1, 0, VK_FORMAT_R16_SFLOAT}; return true;
        case 31: *o = {4, 1, 0, VK_FORMAT_R16G16_SFLOAT}; return true;
        case 32: *o = {8, 1, 0, VK_FORMAT_R16G16B16A16_SFLOAT}; return true;
        case 36: *o = {4, 1, 0, VK_FORMAT_R32_SFLOAT}; return true;
        case 37: *o = {8, 1, 0, VK_FORMAT_R32G32_SFLOAT}; return true;
        case 38: *o = {16, 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT}; return true;
        default: return false;
    }
}
uint32_t Log2Plafond(uint32_t v) { return v <= 1 ? 0 : 32 - __builtin_clz(v - 1); }
uint32_t Log2Plancher(uint32_t v) { return 31 - __builtin_clz(v); }
uint32_t Pow2(uint32_t v) { return 1u << Log2Plafond(v); }
uint32_t Aligner(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

// xenia texture_util.cc GetPackedMipOffset (2D) : décalage du niveau `mip` dans la queue empaquetée, en texels ;
// false si le niveau n'est pas empaqueté.
bool DecalageEmpaquete(uint32_t w, uint32_t h, uint32_t mip, uint32_t* x, uint32_t* y) {
    const uint32_t lw = Log2Plafond(w), lh = Log2Plafond(h), ls = std::min(lw, lh);
    *x = *y = 0;
    if (ls > 4 + mip) return false;
    const uint32_t base = ls > 4 ? ls - 4 : 0, p = mip - base;
    if (p < 3) {
        (lw > lh ? *y : *x) = 16 >> p;
    } else if (lw > lh) {
        *x = (1u << (lw - base)) >> (p - 2);
    } else {
        *y = (1u << (lh - base)) >> (p - 2);
    }
    return true;
}
}  // namespace

bool Disposer(const uint32_t f[6], Disposition* d) {
    *d = Disposition{};
    Format fm;
    if ((f[0] & 3) != 2 || !FormatDe(f[1] & 0x3F, &fm)) return false;
    if (((f[5] >> 9) & 3) != 1) return false;                         // 2D seulement (le jeu n'échantillonne ni cube ni 3D)
    if (((f[1] >> 10) & 1) && ((f[2] >> 26) & 0x3F)) return false;   // tableau de textures
    d->format = f[1] & 0x3F;
    d->endian = (f[1] >> 6) & 3;
    d->bpe = fm.bpe;
    d->bloc = fm.bloc;
    d->vk = fm.vk;
    d->conversion = fm.conversion;
    d->tuile = f[0] >> 31;
    const uint32_t w = (f[2] & 0x1FFF) + 1, h = ((f[2] >> 13) & 0x1FFF) + 1;
    const uint32_t base = f[1] & 0x1FFFF000u, mips = f[5] & 0x1FFFF000u;
    if (!base) return false;   // shortcut: texture sans niveau 0 en mémoire (mip_min_level > 0) non gérée
    // Niveaux (xenia GetSubresourcesFromFetchConstant) ; shortcut: mip_min_level ignoré (niveaux 0..max servis)
    uint32_t max = 0;
    if (mips) max = std::min((f[4] >> 6) & 15, Log2Plancher(std::max(w, h)));
    d->niveaux = std::min(max + 1, kNiveauxMax);
    const uint32_t empaquete = (f[5] >> 11) & 1 ? (Log2Plafond(std::min(w, h)) > 4 ? Log2Plafond(std::min(w, h)) - 4 : 0) : UINT32_MAX;
    const uint32_t b = d->bloc;
    auto pasTuile = [&](uint32_t texels) { return Aligner(Aligner(texels, b) / b, 32); };
    // Niveau 0 : pas de la constante ; mips : tailles arrondies à la puissance de 2 (xenia GetGuestTextureLayout)
    // pas du niveau 0 comme DecodeTexture (pas de la constante, sinon largeur)
    const uint32_t pitch = ((f[0] >> 22) & 0x1FF) * 32;
    uint32_t pas0 = (pitch ? pitch : w) / b;
    if (d->tuile) pas0 = Aligner(pas0, 32);
    auto pasMip = [&](uint32_t l) {   // en éléments ; linéaire : rangées alignées sur 256 octets
        const uint32_t o = pasTuile(std::max(Pow2(w) >> l, 1u)) * d->bpe;
        return (d->tuile ? o : Aligner(o, 256)) / d->bpe;
    };
    uint32_t decalage = 0, adrQueue = 0, pasQueue = 0;
    for (uint32_t l = 0; l < d->niveaux; l++) {
        Niveau& n = d->n[l];
        n.w = std::max(w >> l, 1u);
        n.h = std::max(h >> l, 1u);
        if (l == 0) {
            n.adresse = kPhys | base;
            n.pas = pas0;
        } else if (empaquete == 0) {   // queue des mips à l'adresse des mips, pas d'un niveau 0 en puissance de 2
            n.adresse = kPhys | mips;
            n.pas = pasMip(0);
        } else if (l > empaquete) {    // dans la queue du niveau `empaquete`
            n.adresse = adrQueue;
            n.pas = pasQueue;
        } else {
            n.adresse = kPhys | (mips + decalage);
            n.pas = pasMip(l);
            decalage += Aligner(n.pas * d->bpe * pasTuile(std::max(Pow2(h) >> l, 1u)), 4096);
            adrQueue = n.adresse;
            pasQueue = n.pas;
        }
        if (l >= empaquete) {
            DecalageEmpaquete(w, h, l, &n.ox, &n.oy);
            n.ox /= b;
            n.oy /= b;
        }
        // bornes : dernière rangée d'éléments dans la mémoire physique
        const uint32_t ew = Aligner(n.w, b) / b + n.ox, eh = Aligner(n.h, b) / b + n.oy;
        const uint64_t fin = uint64_t(n.adresse) + uint64_t(Aligner(eh, 32)) * std::max(n.pas, ew) * d->bpe + 0x1000;
        if (fin > uint64_t(kPhys) + kPhysTaille || !n.pas) return false;
    }
    return true;
}

uint32_t TailleNiveau(const Disposition& d, uint32_t l) {
    const Niveau& n = d.n[l];
    const uint32_t ew = (n.w + d.bloc - 1) / d.bloc, eh = (n.h + d.bloc - 1) / d.bloc;
    return ew * eh * (d.conversion == 1 ? 4 : d.bpe);
}

void Convertir(const Disposition& d, uint32_t l, uint8_t* dst) {
    const Niveau& n = d.n[l];
    const uint32_t ew = (n.w + d.bloc - 1) / d.bloc, eh = (n.h + d.bloc - 1) / d.bloc;
    const uint32_t lb = d.bpe == 1 ? 0 : d.bpe == 2 ? 1 : d.bpe == 4 ? 2 : d.bpe == 8 ? 3 : 4;
    // linéaire sans conversion (ex. vidéo Y/U/V) : par mots de 32 bits, si chaque rangée commence sur un mot
    if (!d.tuile && d.conversion == 0 && n.ox == 0 && (n.pas * d.bpe) % 4 == 0) {
        const uint32_t octets = ew * d.bpe;
        for (uint32_t ey = 0; ey < eh; ey++, dst += octets) {
            const uint8_t* src = Host(n.adresse + (ey + n.oy) * n.pas * d.bpe);
            for (uint32_t i = 0; i < octets; i += 4) {
                uint32_t v;
                memcpy(&v, src + i, 4);
                v = gpu::GpuEndianSwap(v, d.endian);
                memcpy(dst + i, &v, std::min(4u, octets - i));
            }
        }
        return;
    }
    auto expand = [](uint32_t v, uint32_t bits) {   // n bits -> 8 bits, arrondi au plus proche
        return uint8_t(bits >= 8 ? v >> (bits - 8) : (v * 255 + ((1u << bits) - 1) / 2) / ((1u << bits) - 1));
    };
    for (uint32_t ey = 0; ey < eh; ey++) {
        const uint32_t y = ey + n.oy;
        const uint32_t ext = d.tuile ? gpu::TiledOffset2DOuter(y, n.pas, lb) : 0;
        for (uint32_t ex = 0; ex < ew; ex++) {
            const uint32_t x = ex + n.ox;
            const uint32_t off = d.tuile ? gpu::TiledOffset2DInner(x, y, lb, ext) : (y * n.pas + x) * d.bpe;
            uint8_t e[16];
            if (d.bpe < 4) {   // texel de 1 ou 2 octets : mode d'octets appliqué au mot de 32 bits qui le contient
                uint32_t v;
                memcpy(&v, Host(n.adresse + (off & ~3u)), 4);
                v = gpu::GpuEndianSwap(v, d.endian);
                memcpy(e, reinterpret_cast<uint8_t*>(&v) + (off & 3), d.bpe);
            } else {
                for (uint32_t i = 0; i < d.bpe; i += 4) {
                    uint32_t v;
                    memcpy(&v, Host(n.adresse + off + i), 4);
                    v = gpu::GpuEndianSwap(v, d.endian);
                    memcpy(e + i, &v, 4);
                }
            }
            if (d.conversion == 0) {
                memcpy(dst, e, d.bpe);
                dst += d.bpe;
            } else if (d.conversion == 1) {
                const uint32_t v = uint32_t(e[0]) | uint32_t(e[1]) << 8;
                if (d.format == 4) { dst[0] = expand(v & 31, 5); dst[1] = expand((v >> 5) & 63, 6); dst[2] = expand((v >> 11) & 31, 5); dst[3] = 255; }
                else if (d.format == 3) { dst[0] = expand(v & 31, 5); dst[1] = expand((v >> 5) & 31, 5); dst[2] = expand((v >> 10) & 31, 5); dst[3] = (v >> 15) & 1 ? 255 : 0; }
                else { dst[0] = expand(v & 15, 4); dst[1] = expand((v >> 4) & 15, 4); dst[2] = expand((v >> 8) & 15, 4); dst[3] = expand((v >> 12) & 15, 4); }
                dst += 4;
            } else {
                uint32_t v;
                memcpy(&v, e, 4);
                const float z = float(v >> 8) / 16777215.0f;   // profondeur sur les 24 bits hauts
                memcpy(dst, &z, 4);
                dst += 4;
            }
        }
    }
}

// ====================== Générations (fil du jeu -> fil natif) ======================
namespace {
std::mutex g_mu;
std::unordered_map<uint32_t, uint32_t> g_gen;   // adresse physique de base -> génération
std::vector<uint32_t> g_detruites;               // bases détruites, évincées au prochain lot
std::atomic<uint32_t> g_epoque{1};               // change à chaque écriture : rien à vérifier tant qu'elle est stable

constexpr uint32_t kTypeTexture = 3;   // D3DCOMMON_TYPE_TEXTURE (XDK), vérifié au journal
uint32_t BaseEnTete(uint32_t res) {
    if (!res) return 0;
    const uint32_t f0 = Ld32(res + 28);
    return (f0 & 3) == 2 ? Ld32(res + 32) & 0x1FFFF000u : 0;   // constante de lecture de texture
}
uint32_t Generation(uint32_t base) {
    std::lock_guard l(g_mu);
    auto it = g_gen.find(base);
    return it == g_gen.end() ? 0 : it->second;
}
}  // namespace

void TextureEcrite(uint32_t tex) {
    const uint32_t b = BaseEnTete(tex);
    if (!b) return;
    {
        static uint32_t vus = 0;
        const uint32_t type = Ld32(tex) & 0xF;
        if (!(vus & (1u << type))) { vus |= 1u << type; fprintf(stderr, "[natif tex] Lock/Unlock : type %u\n", type); }
    }
    std::lock_guard l(g_mu);
    g_gen[b]++;
    g_epoque.fetch_add(1, std::memory_order_release);
}
void TextureDetruite(uint32_t res) {
    // Common (+0) : type de ressource sur les 4 bits bas ; relevé une fois par type pour vérifier la valeur des textures
    const uint32_t type = res ? Ld32(res) & 0xF : 0;
    const uint32_t b = BaseEnTete(res);
    {
        static uint32_t vus = 0;
        if (b && !(vus & (1u << type))) { vus |= 1u << type; fprintf(stderr, "[natif tex] DestructResource : type %u avec constante de texture\n", type); }
    }
    if (!b || type != kTypeTexture) return;
    std::lock_guard l(g_mu);
    g_gen[b]++;
    g_detruites.push_back(b);
    g_epoque.fetch_add(1, std::memory_order_release);
}

// ====================== Cache (fil natif) ======================
namespace {

struct Cle {
    uint32_t base, mips, mot0, mot1, mot2, niveaux;   // mot0 : pas + tuilage ; mot1 : format + mode d'octets
    bool operator==(const Cle&) const = default;
};
struct HacheCle {
    size_t operator()(const Cle& k) const {
        uint64_t h = 1469598103934665603ull;
        for (uint32_t v : {k.base, k.mips, k.mot0, k.mot1, k.mot2, k.niveaux}) h = (h ^ v) * 1099511628211ull;
        return size_t(h);
    }
};
struct Entree {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memoire = VK_NULL_HANDLE;
    std::unordered_map<uint32_t, VkImageView> vues;   // par swizzle
    bool chargee = false;
    uint32_t gen = 0, epoque = 0, lot = 0, derniere = 0;   // `lot` : dernier lot qui l'a liée ; `derniere` : dernière image
};
struct Mort { VkImage image; VkDeviceMemory memoire; std::vector<VkImageView> vues; uint32_t echeance; };   // image où la détruire
struct Stats { uint32_t servies = 0, envois = 0, nonGerees = 0, anneauPlein = 0, echecs = 0; uint64_t octets = 0; double ms = 0; };

std::unordered_map<Cle, Entree, HacheCle> g_cache;
std::vector<Mort> g_morts;
uint32_t g_lot = 1, g_image = 0;
Stats g_s;

void Liberer(Entree& e) {
    Mort m{e.image, e.memoire, {}, g_image + 2};   // 2 images en vol (des lots peuvent partir en cours d'image), cf. Remplacer
    for (auto& [s, v] : e.vues) m.vues.push_back(v);
    g_morts.push_back(std::move(m));
    e.image = VK_NULL_HANDLE;
    e.memoire = VK_NULL_HANDLE;
    e.vues.clear();
}
void Detruire(Mort& m, Context* vk) {
    for (VkImageView v : m.vues) vkDestroyImageView(vk->device, v, nullptr);
    vkDestroyImage(vk->device, m.image, nullptr);
    vkFreeMemory(vk->device, m.memoire, nullptr);
}

bool CreerImage(Context* vk, const Disposition& d, uint32_t w, uint32_t h, Entree* e) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = d.vk;
    ci.extent = {w, h, 1};
    ci.mipLevels = d.niveaux;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (vkCreateImage(vk->device, &ci, nullptr, &e->image) != VK_SUCCESS) { e->image = VK_NULL_HANDLE; return false; }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk->device, e->image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = vk->FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = vk->FindMemory(req.memoryTypeBits, 0);
    if (vkAllocateMemory(vk->device, &ai, nullptr, &e->memoire) != VK_SUCCESS) {
        vkDestroyImage(vk->device, e->image, nullptr);
        e->image = VK_NULL_HANDLE;
        e->memoire = VK_NULL_HANDLE;
        return false;
    }
    vkBindImageMemory(vk->device, e->image, e->memoire, 0);
    return true;
}

bool FormatEchantillonnable(Context* vk, VkFormat f) {
    static std::unordered_map<uint32_t, bool> connus;
    auto it = connus.find(uint32_t(f));
    if (it != connus.end()) return it->second;
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(vk->physical, f, &fp);
    const bool ok = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) &&
                    (f < VK_FORMAT_BC1_RGB_UNORM_BLOCK || f > VK_FORMAT_BC7_SRGB_BLOCK || vk->bc);
    return connus[uint32_t(f)] = ok;
}

VkImageView Vue(Context* vk, Entree& e, const Disposition& d, uint32_t swizzle) {
    VkImageView& v = e.vues[swizzle];
    if (v) return v;
    VkComponentMapping m;
    if (d.conversion == 2) {   // profondeur : x, y, z -> profondeur ; 0 -> 0 ; sinon 1 (comme l'émulation)
        VkComponentSwizzle c[4];
        for (int i = 0; i < 4; i++) {
            const uint32_t s = (swizzle >> (3 * i)) & 7;
            c[i] = s < 3 ? VK_COMPONENT_SWIZZLE_R : s == 4 ? VK_COMPONENT_SWIZZLE_ZERO : VK_COMPONENT_SWIZZLE_ONE;
        }
        m = {c[0], c[1], c[2], c[3]};
    } else {
        static const VkComponentSwizzle sel[8] = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B,
            VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ONE};
        m = {sel[swizzle & 7], sel[(swizzle >> 3) & 7], sel[(swizzle >> 6) & 7], sel[(swizzle >> 9) & 7]};
    }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = e.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = d.vk;
    vi.components = m;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, d.niveaux, 0, 1};
    if (vkCreateImageView(vk->device, &vi, nullptr, &v) != VK_SUCCESS) { e.vues.erase(swizzle); return VK_NULL_HANDLE; }
    return v;
}

void Barriere(VkCommandBuffer cmd, VkImage img, uint32_t niveaux, VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags de,
              VkPipelineStageFlags vers, VkImageLayout avant) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = avant;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, niveaux, 0, 1};
    vkCmdPipelineBarrier(cmd, de, vers, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// Envoi de tous les niveaux par l'anneau de l'image (commandes d'envoi du lot, avant ses draws).
bool Envoyer(Context* vk, const Disposition& d, Entree& e) {
    uint32_t total = 0;
    uint32_t debut[kNiveauxMax];
    for (uint32_t l = 0; l < d.niveaux; l++) { debut[l] = total; total += Aligner(TailleNiveau(d, l), 16); }
    uint8_t* dst = nullptr;
    const ::natif::Buffer src = ::natif::RingUpload(total, &dst);
    if (!dst) { g_s.anneauPlein++; return false; }
    for (uint32_t l = 0; l < d.niveaux; l++) Convertir(d, l, dst + debut[l]);
    const bool neuve = !e.image;
    if (neuve && !CreerImage(vk, d, d.n[0].w, d.n[0].h, &e)) { g_s.echecs++; return false; }
    VkCommandBuffer cmd = CmdEnvoi();
    // ancienne version lue par les lots précédents (déjà soumis, avant dans la file) -> écriture
    Barriere(cmd, e.image, d.niveaux, neuve ? 0 : VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
             neuve ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
             VK_PIPELINE_STAGE_TRANSFER_BIT, neuve ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL);
    VkBufferImageCopy r[kNiveauxMax]{};
    for (uint32_t l = 0; l < d.niveaux; l++) {
        r[l].bufferOffset = src.offset + debut[l];
        r[l].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
        r[l].imageExtent = {d.n[l].w, d.n[l].h, 1};
    }
    vkCmdCopyBufferToImage(cmd, src.buffer, e.image, VK_IMAGE_LAYOUT_GENERAL, d.niveaux, r);
    Barriere(cmd, e.image, d.niveaux, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_GENERAL);
    g_s.envois++;
    g_s.octets += total;
    return true;
}

void Journal(uint32_t image) {
    size_t mips = 0;
    for (const auto& [k, e] : g_cache) mips += k.niveaux > 1;
    fprintf(stderr, "[natif tex %u] servies %u, envois %u (%.1f Mo, %.3f ms CPU/image), en cache %zu (avec mips %zu), non gérées %u, anneau plein %u, "
            "échecs %u\n",
            image, g_s.servies, g_s.envois, g_s.octets / 1048576.0, g_s.ms / 60, g_cache.size(), mips, g_s.nonGerees, g_s.anneauPlein,
            g_s.echecs);
    g_s = Stats{};
}
}  // namespace

VkImageView VueVide() {
    static Entree vide;
    static VkImageView v = VK_NULL_HANDLE;
    if (v) return v;
    Context* vk = Contexte();
    Disposition d;
    d.vk = VK_FORMAT_R8G8B8A8_UNORM;
    d.niveaux = 1;
    if (!vk || !CreerImage(vk, d, 1, 1, &vide)) return VK_NULL_HANDLE;
    VkCommandBuffer cmd = CmdEnvoi();
    Barriere(cmd, vide.image, 1, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
             VK_IMAGE_LAYOUT_UNDEFINED);
    const VkClearColorValue zero{};
    const VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, vide.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &r);
    Barriere(cmd, vide.image, 1, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_IMAGE_LAYOUT_GENERAL);
    v = Vue(vk, vide, d, 0x688);
    return v;
}

void TexturesLot(uint32_t image) {
    Context* vk = Contexte();
    g_lot++;
    for (size_t i = 0; i < g_morts.size();)
        if (int32_t(image - g_morts[i].echeance) >= 0) { Detruire(g_morts[i], vk); g_morts[i] = std::move(g_morts.back()); g_morts.pop_back(); } else i++;
    std::vector<uint32_t> detruites;
    {
        std::lock_guard l(g_mu);
        detruites.swap(g_detruites);
        for (uint32_t b : detruites) g_gen.erase(b);   // l'entrée évincée ne sera plus comparée
    }
    if (!detruites.empty()) {
        std::sort(detruites.begin(), detruites.end());
        for (auto it = g_cache.begin(); it != g_cache.end();)
            if (std::binary_search(detruites.begin(), detruites.end(), it->first.base)) { Liberer(it->second); it = g_cache.erase(it); }
            else ++it;
    }
    if (image == g_image) return;
    g_image = image;
    if (image % 64 == 0) {   // textures inutilisées depuis 10 s (adresse réutilisée sous une autre clé) : libérées
        for (auto it = g_cache.begin(); it != g_cache.end();) {
            if (image - it->second.derniere > 600) { Liberer(it->second); it = g_cache.erase(it); } else ++it;
        }
    }
    if (image % 60 == 0 && (g_s.servies || g_s.envois)) Journal(image);
}

uint32_t EpoqueTextures() { return g_epoque.load(std::memory_order_acquire); }
VkImageView VueTexture(const uint32_t f[6]) {
    Context* vk = Contexte();
    Disposition d;
    if (!vk || !Disposer(f, &d) || !FormatEchantillonnable(vk, d.vk)) {
        g_s.nonGerees++;
        static bool vus[64] = {};
        if (!vus[f[1] & 0x3F] && (f[0] & 3) == 2) {
            vus[f[1] & 0x3F] = true;
            fprintf(stderr, "[natif tex] non gérée : format %u dimension %u (%ux%u, mips %u) -> texture vide\n", f[1] & 0x3F,
                    (f[5] >> 9) & 3, (f[2] & 0x1FFF) + 1, ((f[2] >> 13) & 0x1FFF) + 1, (f[4] >> 6) & 15);
        }
        return VK_NULL_HANDLE;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const Cle k{f[1] & 0x1FFFF000u, f[5] & 0x1FFFF800u,   // mips + bit 11 (empaquetées)
                 f[0] & 0xFFC00000u, f[1] & 0xFFF, f[2], d.niveaux};
    Entree& e = g_cache[k];
    const uint32_t epoque = g_epoque.load(std::memory_order_acquire);
    bool valide = e.image && e.chargee;
    if (valide && e.epoque != epoque) {
        valide = Generation(k.base) == e.gen;
        e.epoque = epoque;
    }
    if (!valide) {
        if (e.image && e.lot == g_lot) Liberer(e);   // déjà liée dans ce lot : nouvelle image, l'ancienne reste aux draws
        e.gen = Generation(k.base);
        e.epoque = epoque;
        e.chargee = Envoyer(vk, d, e);
        if (!e.chargee) {
            g_s.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            return VK_NULL_HANDLE;
        }
        g_s.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    e.lot = g_lot;
    e.derniere = g_image;
    g_s.servies++;
    return Vue(vk, e, d, (f[3] >> 1) & 0xFFF);
}

}  // namespace wk::natif
