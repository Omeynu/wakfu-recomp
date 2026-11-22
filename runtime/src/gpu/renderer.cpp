// Rendu Vulkan des draws Xenos (jalon 4, première version).
// Modèle : la mémoire physique invitée (512 Mo en 0xA0000000) est importée telle quelle et lue par les
// shaders ; une image Vulkan par cible de rendu EDRAM (base, format, pas) ; resolve = relecture de
// l'image puis écriture tuilée en mémoire invitée. Mise en page GENERAL partout. Soumission asynchrone : lots
// soumis aux barrières et en fin d'image sans attente (3 en vol au plus), écritures visibles du CPU différées
// à la fin réelle de leur lot (RendererDefer).
#include "../trace.h"
#include "../config.h"
#include "renderer.h"
#define XXH_INLINE_ALL
#include <xxhash.h>
#include "glsl_translator.h"
#include <cmath>
#include <iterator>
#include <fstream>
#include <filesystem>
#include <deque>
#include <functional>
#include <algorithm>
#include <array>
#include <unordered_map>
#include <cstdio>
#include <set>
#include <cstdlib>
#ifndef _WIN32
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <cstring>
#include <map>
#include <mutex>
#include <memory>
#include <tuple>
#include "../guest.h"
#include "../platform.h"
#include "shader_cache.h"
#include "texture_decode.h"
#include "tiling.h"
#include "vk.h"
#include "xenia/gpu/registers.h"

namespace wk::gpu {

using namespace wk::vk;
namespace reg = xe::gpu::reg;

namespace {
constexpr uint32_t kPhysBase = 0xA0000000u, kPhysSize = 0x20000000u;
constexpr uint32_t REG_CONSTS = 0x4000, REG_FETCH = 0x4800, REG_BOOL = 0x4900, REG_LOOP = 0x4908;

struct Buffer { VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; uint8_t* map = nullptr; VkDeviceSize size = 0; };

// Clés des caches de pipelines et de textures : entiers juxtaposés, hachés par XXH3 (au lieu de chaînes formatées
// dans des std::map, coûteuses à chaque draw).
using Key5 = std::array<uint64_t, 5>;
using Key3 = std::array<uint64_t, 3>;
struct KeyHash {
    template <size_t N> size_t operator()(const std::array<uint64_t, N>& k) const { return size_t(XXH3_64bits(k.data(), N * 8)); }
};
Key3 TexKey(const TextureInfo& t, bool gpu) {
    return {uint64_t(t.address) | uint64_t(t.format) << 32 | uint64_t(t.endian & 3) << 40 | uint64_t(t.tiled ? 1 : 0) << 42,
            uint64_t(t.width) | uint64_t(t.height) << 16 | uint64_t(t.pitch) << 32, uint64_t(t.swizzle) | uint64_t(gpu ? 1 : 0) << 32};
}

struct State {
    Context vk;
    bool ok = false;
    VkBuffer shared = VK_NULL_HANDLE;          // mémoire physique invitée importée
    VkDeviceMemory sharedMemory = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    Image dummy;
    VkCommandBuffer cmd = VK_NULL_HANDLE;      // lot en cours d'enregistrement (emplacement `cur`)
    // Passe de rendu laissée ouverte entre draws de mêmes cibles (fermée avant toute commande hors passe)
    bool passOpen = false;
    // Effacements reportés (cible -> valeur) : faits par la passe de rendu suivante de la cible (loadOp CLEAR), ou avant
    // toute lecture de la cible (resolve) ; évite une commande et deux barrières complètes par effacement.
    struct PendingClear { VkClearValue value; bool depth; };
    std::unordered_map<VkImage, PendingClear> pendingClears;
    VkImage passRt = VK_NULL_HANDLE, passDs = VK_NULL_HANDLE;
    // Lots : enregistré (cur) puis soumis sans attente ; plusieurs en vol, retirés dans l'ordre à la fin réelle
    // (fence). Chacun a son anneau de constantes, ses descripteurs, ses données d'envoi et les écritures du
    // processeur de commandes visibles du CPU (barrières, interruptions), appliquées seulement à sa fin.
    struct Batch {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        Buffer ring;                           // constantes, indices
        VkDeviceSize ringPos = 0;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        uint32_t sets = 0;
        std::vector<Buffer> staging;
        std::vector<std::pair<uint32_t, uint32_t>> pendingWrites;   // plages invitées écrites par le GPU
        uint64_t constGen = 0;                 // génération des constantes copiées dans l'anneau (0 : aucune)
        VkDeviceSize constOff = 0, fetchOff = 0;
        VkDescriptorSet lastSet = VK_NULL_HANDLE;   // dernier jeu des draws et ses textures (réutilisé si identiques)
        VkImageView lastViews[32] = {};
        std::vector<std::function<void()>> ops;
        VkQueryPool times = VK_NULL_HANDLE;   // horodatages début/fin du lot (temps GPU, statistiques)
        uint32_t profN = 0;                    // WAKFU_GPU_PROFIL : horodatages après chaque segment (catégorie)
        uint8_t profCat[1024];
    };
    static constexpr uint32_t kBatches = 3;
    Batch batches[kBatches];
    uint32_t cur = 0;
    std::deque<uint32_t> inFlight;             // emplacements soumis, du plus ancien au plus récent
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, Image> targets;        // (base, format, pas) -> image
    std::map<std::pair<uint32_t, uint32_t>, Image> depths;                    // (base EDRAM, pas) -> profondeur + stencil
    std::map<VkImage, VkImageView> depthSampleViews;                          // vue profondeur seule (resolve)
    std::map<uint64_t, VkShaderModule> modules;
    std::unordered_map<Key5, VkPipeline, KeyHash> pipelines;   // (vs, ps, topologie+drapeaux, mélange+masque, profondeur)
    struct Tex { Image img; uint32_t frame = 0; uint64_t hash = 0, serial = 0; };   // serial : WatchSerial au dernier contrôle   // hash : empreinte CPU, ou numéro d'écriture de la source au décodage GPU
    std::unordered_map<Key3, Tex, KeyHash> textures;           // (adresse, format, …) (taille, pas) (swizzle, GPU)
    VkDescriptorSetLayout resolveSetLayout = VK_NULL_HANDLE;   // resolve sur GPU (calcul)
    VkPipelineLayout resolveLayout = VK_NULL_HANDLE;
    VkPipeline resolvePipe = VK_NULL_HANDLE;
    bool resolveTried = false;
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;   // relu de <cache>/pipelines.bin, réécrit toutes les 16 créations
    uint32_t pipelinesSinceSave = 0;
    uint32_t* waitCause = nullptr;   // compteur de cause des attentes bloquantes en cours (statistiques)
    // Décodage GPU des textures (lues dans la mémoire invitée par un calcul)
    VkDescriptorSetLayout texSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout texLayout = VK_NULL_HANDLE;
    VkPipeline texPipe = VK_NULL_HANDLE, texPipeDepth = VK_NULL_HANDLE;
    bool texTried = false;
    uint32_t frame = 0;
    RendererFrameStats stats;
};
State* g = nullptr;
}  // namespace
}  // namespace wk::gpu
namespace wk { extern uint64_t g_cpConstGen; }   // kernel/gpu_cp.cpp
namespace wk::gpu {
namespace {
std::recursive_mutex g_lock;   // l'API n'est appelée que par le thread du processeur de commandes ; sûreté

bool CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, Buffer* b) {
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    if (vkCreateBuffer(g->vk.device, &ci, nullptr, &b->buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g->vk.device, b->buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = g->vk.FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(g->vk.device, &ai, nullptr, &b->memory) != VK_SUCCESS) return false;
    vkBindBufferMemory(g->vk.device, b->buffer, b->memory, 0);
    void* p;
    vkMapMemory(g->vk.device, b->memory, 0, size, 0, &p);
    b->map = static_cast<uint8_t*>(p);
    b->size = size;
    return true;
}
void DestroyBuffer(Buffer& b) {
    if (b.buffer) vkDestroyBuffer(g->vk.device, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(g->vk.device, b.memory, nullptr);
    b = Buffer{};
}
constexpr uint32_t kMaxSets = 4096;
State::Batch& Cur() { return g->batches[g->cur]; }
// Réserve `size` octets dans l'anneau du lot courant (aligné) ; retourne le décalage, ou UINT64_MAX si plein.
VkDeviceSize RingAlloc(VkDeviceSize size, VkDeviceSize align) {
    State::Batch& b = Cur();
    VkDeviceSize o = (b.ringPos + align - 1) / align * align;
    if (o + size > b.ring.size) return UINT64_MAX;
    b.ringPos = o + size;
    return o;
}

void Barrier(VkCommandBuffer cmd);
bool GpuProfil() { static const bool on = getenv("WAKFU_GPU_PROFIL") && *getenv("WAKFU_GPU_PROFIL") == '1'; return on; }
// Profil GPU : horodatage de fin du segment en cours (0 draws, 1 décodage, 2 envoi, 3 resolve, 4 effacement).
void GpuMark(uint8_t cat) {
    if (!GpuProfil() || !g->cmd) return;
    State::Batch& b = Cur();
    if (!b.times || b.profN >= 1024) return;
    vkCmdWriteTimestamp(g->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, b.times, 2 + b.profN);
    b.profCat[b.profN++] = cat;
}
void ClosePass();
VkCommandBuffer Cmd();
// Effacement reporté de `img` fait tout de suite (avant une lecture de la cible).
void ApplyPendingClear(VkImage img) {
    auto it = g->pendingClears.find(img);
    if (it == g->pendingClears.end()) return;
    VkCommandBuffer cmd = Cmd();
    Barrier(cmd);
    if (it->second.depth) {
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(cmd, img, VK_IMAGE_LAYOUT_GENERAL, &it->second.value.depthStencil, 1, &r);
    } else {
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_GENERAL, &it->second.value.color, 1, &r);
    }
    Barrier(cmd);
    GpuMark(4);
    g->pendingClears.erase(it);
}
// Ferme la passe de rendu ouverte (fin + barrière) : avant toute commande qui ne s'enregistre pas dans une passe.
void ClosePass() {
    if (!g->passOpen) return;
    vkCmdEndRendering(g->cmd);
    Barrier(g->cmd);
    g->passOpen = false;
    GpuMark(0);
}
// Tampon de commandes du lot courant, passe de rendu fermée (CmdForDraw : passe laissée telle quelle).
VkCommandBuffer CmdForDraw() {
    if (!g->cmd) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = g->vk.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(g->vk.device, &ai, &g->cmd);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(g->cmd, &bi);
        if (Cur().times) {
            vkCmdResetQueryPool(g->cmd, Cur().times, 0, 2 + (GpuProfil() ? 1024 : 0));
            Cur().profN = 0;
            vkCmdWriteTimestamp(g->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, Cur().times, 0);
        }
        // Les lots se suivent dans la file sans dépendance mémoire implicite : barrière complète en tête de lot
        // (resolves, cibles et textures du lot précédent).
        Barrier(g->cmd);
    }
    return g->cmd;
}
VkCommandBuffer Cmd() {
    VkCommandBuffer c = CmdForDraw();
    ClosePass();
    return c;
}
// Après une attente GPU : rend visibles au processeur les écritures du GPU dans la mémoire physique exportée.
void SyncPhysicalForCpu();
// Retire le plus ancien lot en vol : attend sa fence (bloquant) si `wait`, sinon seulement s'il est fini ;
// libère ses ressources puis applique ses écritures différées. Retourne faux si rien n'a été retiré.
bool RetireOldest(bool wait) {
    if (g->inFlight.empty()) return false;
    State::Batch& b = g->batches[g->inFlight.front()];
    if (wait && vkGetFenceStatus(g->vk.device, b.fence) != VK_SUCCESS) {
        g->stats.waits++;
        if (g->waitCause) (*g->waitCause)++;
        vkWaitForFences(g->vk.device, 1, &b.fence, VK_TRUE, UINT64_MAX);
    }
    else if (vkGetFenceStatus(g->vk.device, b.fence) != VK_SUCCESS) return false;
    g->inFlight.pop_front();
    trace::Mark('R', uint32_t(b.ops.size()));
    uint64_t ts[2 + 1024] = {};
    const uint32_t nq = 2 + b.profN;
    if (b.times && vkGetQueryPoolResults(g->vk.device, b.times, 0, nq, nq * 8, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] > ts[0]) {
        const double us = g->vk.timestampPeriod / 1000.0;
        g->stats.gpuUs += uint32_t(double(ts[1] - ts[0]) * us);
        uint64_t prev = ts[0];   // segment terminé par chaque horodatage : attribué à sa catégorie
        for (uint32_t i = 0; i < b.profN; i++) {
            if (ts[2 + i] > prev) g->stats.gpuCatUs[b.profCat[i]] += uint32_t(double(ts[2 + i] - prev) * us);
            prev = std::max(prev, ts[2 + i]);
        }
        if (b.profN && ts[1] > prev) g->stats.gpuCatUs[5] += uint32_t(double(ts[1] - prev) * us);
    }
    SyncPhysicalForCpu();
    vkFreeCommandBuffers(g->vk.device, g->vk.pool, 1, &b.cmd);
    b.cmd = VK_NULL_HANDLE;
    for (auto& s : b.staging) DestroyBuffer(s);
    b.staging.clear();
    b.pendingWrites.clear();
    b.ringPos = 0;
    b.constGen = 0;
    b.sets = 0;
    vkResetDescriptorPool(g->vk.device, b.pool, 0);
    b.lastSet = VK_NULL_HANDLE;
    std::vector<std::function<void()>> ops;
    ops.swap(b.ops);
    for (auto& op : ops) op();   // dans l'ordre du flux
    return true;
}
// Soumet le lot courant sans attendre ; passe à l'emplacement suivant (en attendant le plus ancien s'il est occupé).
void Submit() {
    if (!g->cmd) return;
    ClosePass();
    State::Batch& b = Cur();
    if (b.times) vkCmdWriteTimestamp(g->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, b.times, 1);
    vkEndCommandBuffer(g->cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g->cmd;
    vkResetFences(g->vk.device, 1, &b.fence);
    vkQueueSubmit(g->vk.queue, 1, &si, b.fence);
    trace::Mark('U', uint32_t(b.ops.size()));
    g->stats.batches++;
    b.cmd = g->cmd;
    g->cmd = VK_NULL_HANDLE;
    g->inFlight.push_back(g->cur);
    g->cur = (g->cur + 1) % State::kBatches;
    uint32_t* const cause = g->waitCause;
    g->waitCause = &g->stats.waitsRing;
    while (!g->inFlight.empty() && g->inFlight.front() == g->cur) RetireOldest(true);
    g->waitCause = cause;
}
// Soumet et attend tout le travail GPU enregistré (écritures différées comprises).
void Flush() {
    Submit();
    while (RetireOldest(true)) {}
}
// Soumet le lot courant si son anneau ou ses descripteurs ne peuvent pas contenir `bytes` octets et `sets` jeux.
void EnsureSpace(VkDeviceSize bytes, uint32_t sets) {
    const State::Batch& b = Cur();
    if (b.ringPos + bytes + 4 * 256 > b.ring.size || b.sets + sets > kMaxSets) Submit();
}
void Barrier(VkCommandBuffer cmd) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, nullptr, 0, nullptr);
}

Image* Target(uint32_t colorInfo, uint32_t surfacePitch) {
    reg::RB_COLOR_INFO ci;
    ci.value = colorInfo;
    const uint32_t base = ci.color_base, fmt = uint32_t(ci.color_format), pitch = surfacePitch ? surfacePitch : 1280;
    auto key = std::make_tuple(base, fmt, pitch);
    auto it = g->targets.find(key);
    if (it != g->targets.end()) return &it->second;
    Image img;
    if (!g->vk.CreateImage(pitch, 2048 * uint32_t(config::g.renderScale), VK_FORMAT_R8G8B8A8_UNORM,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                               VK_IMAGE_USAGE_SAMPLED_BIT, &img))
        return nullptr;
    g->vk.Submit([&](VkCommandBuffer c) {
        Context::Transition(c, img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        VkClearColorValue z{};
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(c, img.image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &r);
    });
    fprintf(stderr, "[rendu] cible EDRAM base %u format %u pas %u\n", base, fmt, pitch);
    return &(g->targets[key] = img);
}

// MSAA Xenos : en EDRAM, un pixel 2X occupe 1x2 échantillons, un pixel 4X 2x2 (cf. Xenia). Une surface MSAA est donc
// rendue dans l'image 1X de même base, coordonnées multipliées ; D3D s'en sert pour effacer vite (quad 4X demi-taille).
// Échantillons par pixel de la surface : MSAA du jeu (2X : 1x2, 4X : 2x2) multiplié par l'échelle de rendu interne
// (option echelle_rendu) ; les cibles, viewports et scissors sont agrandis d'autant et le resolve fait la moyenne.
void MsaaScale(uint32_t surfaceInfo, uint32_t* sx, uint32_t* sy) {
    const uint32_t m = (surfaceInfo >> 16) & 3, s = uint32_t(config::g.renderScale);
    *sx = (m == 2 ? 2 : 1) * s;
    *sy = (m >= 1 ? 2 : 1) * s;
}
// PA_SC_WINDOW_OFFSET (x, y signés sur 15 bits) : décalage écran -> EDRAM (rendu par bandes de D3D).
void WindowOffset(const uint32_t* regs, int32_t* wx, int32_t* wy) {
    const uint32_t v = regs[0x2080];
    *wx = int32_t((v & 0x7FFF) << 17) >> 17;
    *wy = int32_t(((v >> 16) & 0x7FFF) << 17) >> 17;
}
// Scissor de fenêtre (PA_SC_WINDOW_SCISSOR_TL/BR), décalé sauf si window_offset_disable (bit 31 de TL), borné à 0.
void WindowScissor(const uint32_t* regs, int32_t* x0, int32_t* y0, int32_t* x1, int32_t* y1) {
    const uint32_t tl = regs[0x2081], br = regs[0x2082];
    *x0 = int32_t(tl & 0x3FFF); *y0 = int32_t((tl >> 16) & 0x3FFF);
    *x1 = int32_t(br & 0x3FFF); *y1 = int32_t((br >> 16) & 0x3FFF);
    if (!(tl >> 31)) {
        int32_t wx, wy;
        WindowOffset(regs, &wx, &wy);
        *x0 += wx; *x1 += wx; *y0 += wy; *y1 += wy;
    }
    *x0 = std::max(*x0, 0); *y0 = std::max(*y0, 0); *x1 = std::max(*x1, 0); *y1 = std::max(*y1, 0);
}
// Rectangle d'un resolve (cf. Xenia) : les 3 sommets du draw de resolve (constante de fetch 0, x/y flottants), décalés
// par la fenêtre si vtx_window_offset_enable, bornés au scissor. Coordonnées EDRAM (pixels de la surface).
bool ResolveRect(const uint32_t* regs, uint32_t* rx0, uint32_t* ry0, uint32_t* rx1, uint32_t* ry1) {
    const uint32_t f0 = regs[REG_FETCH], f1 = regs[REG_FETCH + 1];
    if ((f0 & 3) != 3 || ((f1 >> 2) & 0xFFFFFF) < 6) return false;   // constante de sommets, 6 mots
    const uint32_t a = f0 & ~3u, ga = a < kPhysSize ? kPhysBase | a : a;
    float v[6];
    for (int i = 0; i < 6; i++) {
        const uint32_t w = GpuEndianSwap(__builtin_bswap32(Ld32(ga + 4 * i)), f1 & 3);
        memcpy(&v[i], &w, 4);
        if (!std::isfinite(v[i])) return false;
    }
    int32_t x0 = int32_t(std::lround(std::min({v[0], v[2], v[4]}))), y0 = int32_t(std::lround(std::min({v[1], v[3], v[5]})));
    int32_t x1 = int32_t(std::lround(std::max({v[0], v[2], v[4]}))), y1 = int32_t(std::lround(std::max({v[1], v[3], v[5]})));
    if ((regs[0x2205] >> 16) & 1) {
        int32_t wx, wy;
        WindowOffset(regs, &wx, &wy);
        x0 += wx; x1 += wx; y0 += wy; y1 += wy;
    }
    int32_t sx0, sy0, sx1, sy1;
    WindowScissor(regs, &sx0, &sy0, &sx1, &sy1);
    x0 = std::clamp(x0, sx0, sx1); x1 = std::clamp(x1, sx0, sx1);
    y0 = std::clamp(y0, sy0, sy1); y1 = std::clamp(y1, sy0, sy1);
    *rx0 = uint32_t(x0); *ry0 = uint32_t(y0); *rx1 = uint32_t(x1); *ry1 = uint32_t(y1);
    return true;
}
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;   // D24S8 / D24FS8 Xenos ; pris en charge partout

// Profondeur + stencil de l'EDRAM (RB_DEPTH_INFO), une image par base et pas ; effacée à 1 / 0 à la création.
Image* DepthTarget(uint32_t depthInfo, uint32_t surfacePitch) {
    const uint32_t base = depthInfo & 0xFFF, pitch = surfacePitch ? surfacePitch : 1280;
    auto key = std::make_pair(base, pitch);
    auto it = g->depths.find(key);
    if (it != g->depths.end()) return &it->second;
    Image img;
    if (!g->vk.CreateImage(pitch, 2048 * uint32_t(config::g.renderScale), kDepthFormat,
                           VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &img))
        return nullptr;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = kDepthFormat;
    vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    VkImageView dv = VK_NULL_HANDLE;
    if (vkCreateImageView(g->vk.device, &vi, nullptr, &dv) == VK_SUCCESS) g->depthSampleViews[img.image] = dv;
    g->vk.Submit([&](VkCommandBuffer c) {
        const VkImageAspectFlags a = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        Context::Transition(c, img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, a);
        VkClearDepthStencilValue z{1.0f, 0};
        VkImageSubresourceRange r{a, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(c, img.image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &r);
    });
    fprintf(stderr, "[rendu] profondeur EDRAM base %u pas %u\n", base, pitch);
    return &(g->depths[key] = img);
}

VkShaderModule Module(uint64_t hash, const std::vector<uint32_t>& spirv) {
    auto it = g->modules.find(hash);
    if (it != g->modules.end()) return it->second;
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = spirv.size() * 4;
    ci.pCode = spirv.data();
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(g->vk.device, &ci, nullptr, &m);
    return g->modules[hash] = m;
}

VkBlendFactor Factor(uint32_t f) {
    switch (f) {
        case 0: return VK_BLEND_FACTOR_ZERO; case 1: return VK_BLEND_FACTOR_ONE;
        case 4: return VK_BLEND_FACTOR_SRC_COLOR; case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 6: return VK_BLEND_FACTOR_SRC_ALPHA; case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 8: return VK_BLEND_FACTOR_DST_COLOR; case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 10: return VK_BLEND_FACTOR_DST_ALPHA; case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR; case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA; case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        default: return VK_BLEND_FACTOR_ONE;
    }
}
VkBlendOp Op(uint32_t o) {
    switch (o) { case 1: return VK_BLEND_OP_SUBTRACT; case 2: return VK_BLEND_OP_MIN; case 3: return VK_BLEND_OP_MAX;
                 case 4: return VK_BLEND_OP_REVERSE_SUBTRACT; default: return VK_BLEND_OP_ADD; }
}

// Rectangles Xenos (primitive 8) : 3 sommets par rectangle ; le 4e est calculé comme Xenia (côté le plus long =
// diagonale, 4e = extrémités de la diagonale - sommet de l'angle), positions et interpolateurs.
const char* kRectGs = R"(#version 450
layout(triangles) in;
layout(triangle_strip, max_vertices = 4) out;
in gl_PerVertex { vec4 gl_Position; float gl_PointSize; } gl_in[];
layout(location = 0) in vec4 xi[][14];         // 14 interpolateurs : limite de 64 composantes en entrée
layout(location = 0) out vec4 xo[16];
void emitv(int i) { gl_Position = gl_in[i].gl_Position; for (int k = 0; k < 14; k++) xo[k] = xi[i][k]; xo[14] = xo[15] = vec4(0.0); EmitVertex(); }
void main() {
    vec2 p0 = gl_in[0].gl_Position.xy / gl_in[0].gl_Position.w;
    vec2 p1 = gl_in[1].gl_Position.xy / gl_in[1].gl_Position.w;
    vec2 p2 = gl_in[2].gl_Position.xy / gl_in[2].gl_Position.w;
    float d01 = dot(p1 - p0, p1 - p0), d12 = dot(p2 - p1, p2 - p1), d20 = dot(p0 - p2, p0 - p2);
    int c = 2, a = 0, b = 1;                       // c : angle ; a, b : diagonale
    if (d12 >= d01 && d12 >= d20) { c = 0; a = 1; b = 2; }
    else if (d20 >= d01 && d20 >= d12) { c = 1; a = 2; b = 0; }
    emitv(c); emitv(a); emitv(b);
    gl_Position = gl_in[a].gl_Position + gl_in[b].gl_Position - gl_in[c].gl_Position;
    for (int k = 0; k < 14; k++) xo[k] = xi[a][k] + xi[b][k] - xi[c][k];
    xo[14] = xo[15] = vec4(0.0);
    EmitVertex();
    EndPrimitive();
}
)";

VkShaderModule RectGeometryModule() {
    static VkShaderModule mod = VK_NULL_HANDLE;
    static bool tried = false;
    if (tried) return mod;
    tried = true;
    std::vector<uint32_t> spv;
    std::string log;
    if (!g->vk.geometryShader || !CompileGlsl(3, kRectGs, &spv, &log)) {
        fprintf(stderr, "[rendu] rectangles approchés (shader de géométrie indisponible) %s\n", log.c_str());
        return mod;
    }
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = spv.size() * 4;
    ci.pCode = spv.data();
    vkCreateShaderModule(g->vk.device, &ci, nullptr, &mod);
    return mod;
}

// Cache de pipelines Vulkan sur disque : le pilote valide l'en-tête (fournisseur, appareil, UUID) et ignore un
// cache d'un autre GPU ou pilote. Enregistré par fichier temporaire + renommage (le jeu quitte sans prévenir).
void LoadPipelineCache() {
    std::vector<char> data;
    const std::string dir = ShaderCacheDir();
    if (!dir.empty()) {
        std::ifstream f(dir + "/pipelines.bin", std::ios::binary);
        data.assign(std::istreambuf_iterator<char>(f), {});
    }
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = data.size();
    ci.pInitialData = data.empty() ? nullptr : data.data();
    if (vkCreatePipelineCache(g->vk.device, &ci, nullptr, &g->pipelineCache) != VK_SUCCESS) {
        ci.initialDataSize = 0;   // données refusées : cache vide
        ci.pInitialData = nullptr;
        vkCreatePipelineCache(g->vk.device, &ci, nullptr, &g->pipelineCache);
    }
    if (!data.empty()) fprintf(stderr, "[rendu] cache de pipelines relu (%zu Ko)\n", data.size() / 1024);
}
void SavePipelineCache() {
    g->pipelinesSinceSave = 0;
    const std::string dir = ShaderCacheDir();
    if (dir.empty() || !g->pipelineCache) return;
    size_t n = 0;
    if (vkGetPipelineCacheData(g->vk.device, g->pipelineCache, &n, nullptr) != VK_SUCCESS || !n) return;
    std::vector<char> data(n);
    if (vkGetPipelineCacheData(g->vk.device, g->pipelineCache, &n, data.data()) != VK_SUCCESS) return;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string path = dir + "/pipelines.bin", tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        f.write(data.data(), std::streamsize(n));
        if (!f) return;
    }
    std::filesystem::rename(tmp, path, ec);
}

VkPipeline Pipeline(uint64_t vs, uint64_t ps, VkPrimitiveTopology topo, uint32_t blend, uint32_t mask, uint32_t depthCtl, bool hasDepth, bool rect) {
    VkShaderModule gsMod = rect ? RectGeometryModule() : VK_NULL_HANDLE;
    const Key5 key{vs, ps, uint64_t(topo) | uint64_t(hasDepth ? 1 : 0) << 32 | uint64_t(gsMod != VK_NULL_HANDLE ? 1 : 0) << 33,
                   uint64_t(blend) | uint64_t(mask) << 32, hasDepth ? depthCtl : 0};
    auto it = g->pipelines.find(key);
    if (it != g->pipelines.end()) return it->second;
    std::vector<uint32_t> vspv, pspv, tu;
    if (!ShaderCacheGetSpirv(vs, &vspv, &tu) || !ShaderCacheGetSpirv(ps, &pspv, &tu)) return g->pipelines[key] = VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st[3]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                          {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = Module(vs, vspv); st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = Module(ps, pspv); st[1].pName = "main";
    st[2].stage = VK_SHADER_STAGE_GEOMETRY_BIT; st[2].module = gsMod; st[2].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = topo;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    reg::RB_BLENDCONTROL bc;
    bc.value = blend;
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = mask & 0xF;
    ba.srcColorBlendFactor = Factor(uint32_t(bc.color_srcblend));
    ba.dstColorBlendFactor = Factor(uint32_t(bc.color_destblend));
    ba.colorBlendOp = Op(uint32_t(bc.color_comb_fcn));
    ba.srcAlphaBlendFactor = Factor(uint32_t(bc.alpha_srcblend));
    ba.dstAlphaBlendFactor = Factor(uint32_t(bc.alpha_destblend));
    ba.alphaBlendOp = Op(uint32_t(bc.alpha_comb_fcn));
    ba.blendEnable = !(ba.srcColorBlendFactor == VK_BLEND_FACTOR_ONE && ba.dstColorBlendFactor == VK_BLEND_FACTOR_ZERO &&
                       ba.colorBlendOp == VK_BLEND_OP_ADD && ba.srcAlphaBlendFactor == VK_BLEND_FACTOR_ONE &&
                       ba.dstAlphaBlendFactor == VK_BLEND_FACTOR_ZERO && ba.alphaBlendOp == VK_BLEND_OP_ADD);
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = hasDepth ? 5 : 2;
    ds.pDynamicStates = dyn;
    // Profondeur / stencil (RB_DEPTHCONTROL) : fonctions et opérations Xenos dans l'ordre de Vulkan
    reg::RB_DEPTHCONTROL dc;
    dc.value = depthCtl;
    VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    if (hasDepth) {
        dss.depthTestEnable = dc.z_enable;
        dss.depthWriteEnable = dc.z_enable && dc.z_write_enable;
        dss.depthCompareOp = VkCompareOp(uint32_t(dc.zfunc));
        dss.stencilTestEnable = dc.stencil_enable;
        dss.front = {VkStencilOp(uint32_t(dc.stencilfail)), VkStencilOp(uint32_t(dc.stencilzpass)),
                     VkStencilOp(uint32_t(dc.stencilzfail)), VkCompareOp(uint32_t(dc.stencilfunc)), 0xFF, 0xFF, 0};
        dss.back = dc.backface_enable
                       ? VkStencilOpState{VkStencilOp(uint32_t(dc.stencilfail_bf)), VkStencilOp(uint32_t(dc.stencilzpass_bf)),
                                          VkStencilOp(uint32_t(dc.stencilzfail_bf)), VkCompareOp(uint32_t(dc.stencilfunc_bf)), 0xFF, 0xFF, 0}
                       : dss.front;
    }
    VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &fmt;
    if (hasDepth) ri.depthAttachmentFormat = ri.stencilAttachmentFormat = kDepthFormat;
    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.pNext = &ri;
    pci.stageCount = gsMod ? 3 : 2;
    pci.pStages = st;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pColorBlendState = &cb;
    pci.pDepthStencilState = &dss;
    pci.pDynamicState = &ds;
    pci.layout = g->layout;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g->vk.device, g->pipelineCache, 1, &pci, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    if (p && ++g->pipelinesSinceSave >= 16) SavePipelineCache();
    return g->pipelines[key] = p;
}

// Textures 2D des formats de DecodeTexture / DecodeDepthTexture : décodées par le GPU (kTexDecodeGlsl) directement
// depuis la mémoire invitée, dans le flux de commandes (après les resolves qui les précèdent). Ni attente du GPU, ni
// empreinte CPU, ni envoi. WAKFU_CPU_TEXTURES=1 : désactivé (décodage CPU).
bool TexDecodePipeline() {
    if (g->texTried) return g->texPipe != VK_NULL_HANDLE;
    g->texTried = true;
    if (getenv("WAKFU_CPU_TEXTURES")) return false;
    for (VkFormat f : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32G32B32A32_SFLOAT}) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(g->vk.physical, f, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) return false;
    }
    VkDescriptorSetLayoutBinding b[2] = {};
    b[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 2;
    dl.pBindings = b;
    vkCreateDescriptorSetLayout(g->vk.device, &dl, nullptr, &g->texSetLayout);
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TexDecodeParams)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g->texSetLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pr;
    vkCreatePipelineLayout(g->vk.device, &pl, nullptr, &g->texLayout);
    for (int depth = 0; depth < 2; depth++) {
        std::vector<uint32_t> spv;
        std::string log;
        const std::string src = std::string("#version 450\n") + (depth ? "#define DEPTH\n" : "") + kTexDecodeGlsl;
        if (!CompileGlsl(2, src.c_str(), &spv, &log)) { fprintf(stderr, "[rendu] décodage GPU des textures indisponible : %s\n", log.c_str()); return false; }
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = spv.size() * 4;
        mi.pCode = spv.data();
        VkShaderModule mod = VK_NULL_HANDLE;
        vkCreateShaderModule(g->vk.device, &mi, nullptr, &mod);
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr};
        ci.layout = g->texLayout;
        VkPipeline& pipe = depth ? g->texPipeDepth : g->texPipe;
        if (vkCreateComputePipelines(g->vk.device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipe) != VK_SUCCESS) pipe = VK_NULL_HANDLE;
        vkDestroyShaderModule(g->vk.device, mod, nullptr);
    }
    return g->texPipe != VK_NULL_HANDLE && g->texPipeDepth != VK_NULL_HANDLE;
}

// Vrai si la texture est décodée sur GPU : format géré par le calcul, 2D, plage source dans la mémoire physique.
bool GpuDecodable(const TextureInfo& t) {
    // k_8 linéaire (plans Y/U/V de la vidéo de l'écran-titre, réécrits par le CPU à chaque image) : décodée sur GPU,
    // rayures par bandes de 16 lignes sous RADV (pas sous lavapipe ; décodage identique au CPU d'après
    // wakfu_unit_texdecode, copie au moment du CP sans effet). Cause non élucidée : chemin CPU (empreinte + envoi).
    if (t.format == 2 && !t.tiled) return false;
    TexDecodeParams pc;
    uint32_t gx, gy;
    if (!TexDecodeSetup(t, 0, &pc, &gx, &gy)) return false;
    const uint32_t a = t.address < kPhysSize ? kPhysBase | t.address : t.address;
    const uint64_t span = TextureSourceSpan(t);
    if (!span || a < kPhysBase || uint64_t(a) + span > uint64_t(kPhysBase) + kPhysSize) return false;
    return TexDecodePipeline();
}

}  // namespace

// Présentation (present.cpp) : décodage GPU du tampon d'image 8_8_8_8 dans une image de l'appelant (RGBA8, usage
// STORAGE, mise en page GENERAL), dans son propre tampon de commandes ; `set` alloué par l'appelant avec
// RendererDecodeSetLayout(). La mémoire lue est finale : la présentation a lieu à la fin réelle du lot de l'image.
VkDescriptorSetLayout RendererDecodeSetLayout() {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return RendererActive() && TexDecodePipeline() ? g->texSetLayout : VK_NULL_HANDLE;
}
// Vrai si RendererRecordDecode peut décoder ce tampon d'image (8_8_8_8 2D dans la mémoire physique) ; sinon la
// présentation passe par le CPU.
bool RendererCanDecodeFrame(const TextureInfo& t) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!RendererActive() || !TexDecodePipeline() || t.format != 6 || t.dimension != 1) return false;
    const uint32_t a = t.address < kPhysSize ? kPhysBase | t.address : t.address;
    const uint64_t span = TextureSourceSpan(t);
    return span && a >= kPhysBase && uint64_t(a) + span <= uint64_t(kPhysBase) + kPhysSize;
}
bool RendererRecordDecode(VkCommandBuffer cmd, VkDescriptorSet set, const TextureInfo& t, VkImageView dst) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!RendererCanDecodeFrame(t)) return false;
    const uint32_t a = t.address < kPhysSize ? kPhysBase | t.address : t.address;
    const uint64_t span = TextureSourceSpan(t);
    const VkDeviceSize rel = a - kPhysBase, off = rel & ~VkDeviceSize(255);
    VkDescriptorBufferInfo bi{g->shared, off, std::min<VkDeviceSize>((rel - off) + span + 4, kPhysSize - off)};
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, dst, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2] = {};
    for (int i = 0; i < 2; i++) { w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = set; w[i].dstBinding = uint32_t(i); w[i].descriptorCount = 1; }
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[0].pBufferInfo = &bi;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[1].pImageInfo = &ii;
    vkUpdateDescriptorSets(g->vk.device, 2, w, 0, nullptr);
    TexDecodeParams pc;
    uint32_t gx, gy;
    TexDecodeSetup(t, uint32_t((rel - off) / 4), &pc, &gx, &gy);
    pc.swz = (t.swizzle & 0x1FF) | (5u << 9);   // alpha forcé à 1 (image affichée opaque)
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g->texPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g->texLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, g->texLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cmd, gx, gy, 1);
    return true;
}

namespace {
// Décodage sur GPU ; réutilisé tant que sa source n'a pas été écrite (surveillance des pages, cf. platform::Watch*).
VkImageView GpuDecodedView(VkCommandBuffer cmd, const TextureInfo& t) {
    const Key3 key = TexKey(t, true);
    State::Tex& tex = g->textures[key];
    const uint32_t ta = t.address < kPhysSize ? kPhysBase | t.address : t.address;
    const uint64_t te = uint64_t(ta) + TextureSourceSpan(t);
    // Réutilisée tant qu'aucune page de sa source n'a été écrite depuis son décodage (CPU : pages surveillées ;
    // GPU : resolves, cf. WatchTouch), y compris d'une image à l'autre.
    const uint32_t ps = ta & 0x1FFFFFFFu, pe = uint32_t(std::min<uint64_t>(0x20000000ull, uint64_t(ps) + (te - ta)));
    const uint64_t serial = platform::WatchSerial();   // inchangé depuis le dernier contrôle : rien n'a été écrit
    if (tex.img.image && tex.frame && (tex.serial == serial || platform::WatchGen(ps, pe) == tex.hash)) {
        tex.serial = serial;
        tex.frame = g->frame;
        return tex.img.view;
    }
    ClosePass();   // calcul : hors passe de rendu
    const bool fresh = !tex.img.image, depth = t.format == 22 || t.format == 23;
    if (fresh && !g->vk.CreateImage(t.width, t.height, depth ? VK_FORMAT_R32G32B32A32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM,
                                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, &tex.img)) {
        g->textures.erase(key);
        return g->dummy.view;
    }
    const uint32_t a = t.address < kPhysSize ? kPhysBase | t.address : t.address;
    const VkDeviceSize rel = a - kPhysBase, off = rel & ~VkDeviceSize(255);
    const VkDeviceSize need = (rel - off) + TextureSourceSpan(t) + 4;
    // jeu réservé par RendererDraw (pas de soumission ici : `cmd` et l'anneau du draw resteraient dans l'ancien lot)
    if (Cur().sets >= kMaxSets) return g->dummy.view;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = Cur().pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &g->texSetLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(g->vk.device, &ai, &set) != VK_SUCCESS) return g->dummy.view;
    Cur().sets++;
    VkDescriptorBufferInfo bi{g->shared, off, std::min<VkDeviceSize>(need, kPhysSize - off)};
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, tex.img.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2] = {};
    for (int i = 0; i < 2; i++) { w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = set; w[i].dstBinding = uint32_t(i); w[i].descriptorCount = 1; }
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[0].pBufferInfo = &bi;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[1].pImageInfo = &ii;
    vkUpdateDescriptorSets(g->vk.device, 2, w, 0, nullptr);
    TexDecodeParams pc;
    uint32_t gx, gy;
    TexDecodeSetup(t, uint32_t((rel - off) / 4), &pc, &gx, &gy);
    if (fresh) Context::Transition(cmd, tex.img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    Barrier(cmd);   // après le resolve (et les lectures de l'image par les draws précédents)
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, depth ? g->texPipeDepth : g->texPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g->texLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, g->texLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cmd, gx, gy, 1);
    Barrier(cmd);
    GpuMark(1);
    tex.frame = g->frame;
    platform::WatchProtect(ps, pe);   // avant la lecture du numéro : toute écriture postérieure le change
    tex.serial = platform::WatchSerial();   // avant WatchGen : une écriture entre les deux change le numéro global
    tex.hash = platform::WatchGen(ps, pe);
    g->stats.textureUploads++;
    return tex.img.view;
}

// Texture d'un emplacement de constante : décodée sur GPU à chaque usage, sinon (secours) sur CPU si la mémoire a changé.
VkImageView TextureView(VkCommandBuffer cmd, const uint32_t* regs, uint32_t slot) {
    const uint32_t* f = regs + REG_FETCH + slot * 6;
    const TextureInfo t = ParseTextureFetch(f);
    if (t.type != 2 || !t.address) return g->dummy.view;
    if (GpuDecodable(t)) return GpuDecodedView(cmd, t);
    const Key3 key = TexKey(t, false);
    auto it = g->textures.find(key);
    // Empreinte recalculée à chaque usage, même dans une image : le jeu remplit plusieurs fois par image une même
    // zone mémoire (texture de passage, mêmes adresse, taille et format) ; réutiliser la texture décodée au premier
    // usage de l'image affichait la première version partout (zone 1 : décor de l'écran de chapitre au lieu de la forêt).
    const uint64_t srcHash = TextureSourceHash(t);
    if (it != g->textures.end() && srcHash && it->second.hash == srcHash) {   // mémoire inchangée : pas de nouveau décodage
        it->second.frame = g->frame;
        return it->second.img.view;
    }
    std::vector<uint8_t> rgba;
    std::string why;
    const bool depthTex = t.format == 22 || t.format == 23;
    if (depthTex) {
        std::vector<float> d;
        if (DecodeDepthTexture(t, &d)) { rgba.resize(d.size() * 4); memcpy(rgba.data(), d.data(), rgba.size()); }
        else why = "texture de profondeur non 2D";
    }
    if (depthTex ? rgba.empty() : !DecodeTexture(t, &rgba, &why)) {
        g->stats.textureFailures++;
        static std::set<std::string> reported;   // une ligne par cause
        if (reported.insert(why).second) fprintf(stderr, "[rendu] texture non gérée : %s (%ux%u)\n", why.c_str(), t.width, t.height);
        return g->dummy.view;
    }
    State::Tex& tex = g->textures[key];
    if (!tex.img.image && !g->vk.CreateImage(t.width, t.height, depthTex ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM,
                                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &tex.img)) {
        g->textures.erase(key);
        return g->dummy.view;
    }
    if (depthTex && tex.frame == 0) {   // profondeur : swizzle de la constante par la vue (x, y, z -> profondeur ; w -> 1)
        auto sel = [&](uint32_t c) {
            switch ((t.swizzle >> (3 * c)) & 7) {
                case 0: case 1: case 2: return VK_COMPONENT_SWIZZLE_R;
                case 4: return VK_COMPONENT_SWIZZLE_ZERO;
                default: return VK_COMPONENT_SWIZZLE_ONE;
            }
        };
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = tex.img.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R32_SFLOAT;
        vi.components = {sel(0), sel(1), sel(2), sel(3)};
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView v = VK_NULL_HANDLE;
        if (vkCreateImageView(g->vk.device, &vi, nullptr, &v) == VK_SUCCESS) {
            vkDestroyImageView(g->vk.device, tex.img.view, nullptr);
            tex.img.view = v;
            for (auto& bb : g->batches) bb.lastSet = VK_NULL_HANDLE;   // une nouvelle vue peut reprendre l'adresse de l'ancienne
        }
    }
    ClosePass();   // copie : hors passe de rendu
    // Données envoyées par l'anneau du lot (sans allocation Vulkan, coûteuse sous Windows) ; tampon dédié si plein
    VkBuffer src = VK_NULL_HANDLE;
    VkDeviceSize srcOffset = RingAlloc(rgba.size(), 16);
    if (srcOffset != UINT64_MAX) {
        memcpy(Cur().ring.map + srcOffset, rgba.data(), rgba.size());
        src = Cur().ring.buffer;
    } else {
        Buffer stg;
        if (!CreateBuffer(rgba.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &stg)) return g->dummy.view;
        memcpy(stg.map, rgba.data(), rgba.size());
        Cur().staging.push_back(stg);
        src = stg.buffer;
        srcOffset = 0;
    }
    const bool fresh = tex.frame == 0;
    Context::Transition(cmd, tex.img.image, fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.bufferOffset = srcOffset;
    region.imageExtent = {t.width, t.height, 1};
    vkCmdCopyBufferToImage(cmd, src, tex.img.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    Barrier(cmd);
    GpuMark(2);
    tex.frame = g->frame;
    tex.hash = srcHash;
    g->stats.textureUploads++;
    return tex.img.view;
}

// Linux : la mémoire physique invitée (512 Mo, vue en 0xA0000000 et en miroir en 0xC0000000) est allouée par
// Vulkan, exportée (dma-buf, sinon fd opaque) puis projetée aux deux adresses. Évite l'import de mémoire hôte,
// que amdgpu refuse pour une mémoire partagée (nécessaire au miroir). Appelé avant le code invité.
int g_physFd = -1;
void SyncPhysicalForCpu() {
#ifndef _WIN32
    if (g_physFd < 0) return;
    dma_buf_sync sy{DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW};
    ioctl(g_physFd, DMA_BUF_IOCTL_SYNC, &sy);
    sy.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW;
    ioctl(g_physFd, DMA_BUF_IOCTL_SYNC, &sy);
#endif
}   // dma-buf de la mémoire physique (synchronisation des caches CPU)
bool ExportPhysicalMemory() {
#ifdef _WIN32
    return false;
#else
    if (!g->vk.memoryFdExport) return false;
    auto getFd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(g->vk.device, "vkGetMemoryFdKHR"));
    if (!getFd) return false;
    const VkExternalMemoryHandleTypeFlagBits type = g->vk.dmaBufExport
        ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkExternalMemoryBufferCreateInfo eb{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    eb.handleTypes = type;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.pNext = &eb;
    bci.size = kPhysSize;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    VkBuffer buf = VK_NULL_HANDLE;
    if (vkCreateBuffer(g->vk.device, &bci, nullptr, &buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g->vk.device, buf, &req);
    // Le processeur invité travaille dans cette mémoire : elle doit être en cache côté hôte.
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                       VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    VkExportMemoryAllocateInfo ex{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    ex.handleTypes = type;
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    ded.buffer = buf;
    ex.pNext = &ded;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &ex;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = g->vk.FindMemory(req.memoryTypeBits, want);
    VkDeviceMemory mem = VK_NULL_HANDLE;
    int fd = -1;
    VkMemoryGetFdInfoKHR gi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
    gi.handleType = type;
    bool ok = ai.memoryTypeIndex != UINT32_MAX && vkAllocateMemory(g->vk.device, &ai, nullptr, &mem) == VK_SUCCESS;
    if (ok) { gi.memory = mem; ok = getFd(g->vk.device, &gi, &fd) == VK_SUCCESS && fd >= 0; }
    void* tmp = MAP_FAILED;
    if (ok) tmp = mmap(nullptr, kPhysSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ok = ok && tmp != MAP_FAILED;
    if (ok) {
        memcpy(tmp, Host(kPhysBase), kPhysSize);   // contenu déjà écrit par le noyau (aucun code invité n'a tourné)
        munmap(tmp, kPhysSize);
        for (uint32_t view : {kPhysBase, 0xC0000000u})
            ok = ok && mmap(Host(view), kPhysSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) != MAP_FAILED;
    }
    if (fd >= 0 && !ok) close(fd);
    if (ok) g_physFd = fd;
    if (!ok) {
        // Une projection partielle laisserait les vues désynchronisées : arrêt plutôt que corruption.
        if (tmp != MAP_FAILED && mem) { fprintf(stderr, "[rendu] projection de la mémoire physique échouée\n"); abort(); }
        if (mem) vkFreeMemory(g->vk.device, mem, nullptr);
        vkDestroyBuffer(g->vk.device, buf, nullptr);
        return false;
    }
    vkBindBufferMemory(g->vk.device, buf, mem, 0);
    g->shared = buf;
    g->sharedMemory = mem;
    return true;
#endif
}
}  // namespace

bool RendererInit(std::string* error) {
    if (g) return g->ok;
    g = new State();
    if (!g->vk.Init(error)) return false;
    const bool exported = ExportPhysicalMemory();
    if (!exported) {
    if (!g->vk.hostMemoryImport) { if (error) *error = "VK_EXT_external_memory_host absente"; return false; }
    // Mémoire physique invitée importée (aucune copie)
    void* host = Host(kPhysBase);
    VkMemoryHostPointerPropertiesEXT hp{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    vkGetMemoryHostPointerPropertiesEXT(g->vk.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host, &hp);
    VkExternalMemoryBufferCreateInfo eb{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    eb.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.pNext = &eb;
    bci.size = kPhysSize;
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    vkCreateBuffer(g->vk.device, &bci, nullptr, &g->shared);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g->vk.device, g->shared, &req);
    VkImportMemoryHostPointerInfoEXT imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    imp.pHostPointer = host;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &imp;
    ai.allocationSize = kPhysSize;
    ai.memoryTypeIndex = g->vk.FindMemory(req.memoryTypeBits & hp.memoryTypeBits, 0);
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(g->vk.device, &ai, nullptr, &g->sharedMemory) != VK_SUCCESS) {
        if (error) *error = "import de la mémoire invitée refusé";
        return false;
    }
    vkBindBufferMemory(g->vk.device, g->shared, g->sharedMemory, 0);
    }
    VkDescriptorSetLayoutBinding b[4] = {};
    b[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b[2] = {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b[3] = {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 4;
    dl.pBindings = b;
    vkCreateDescriptorSetLayout(g->vk.device, &dl, nullptr, &g->setLayout);
    VkPushConstantRange pr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 56};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g->setLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pr;
    vkCreatePipelineLayout(g->vk.device, &pl, nullptr, &g->layout);
    VkDescriptorPoolSize sizes[4] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 8192}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096 * 4},
                                     {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096 * 32}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4096}};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = kMaxSets;
    dp.poolSizeCount = 4;
    dp.pPoolSizes = sizes;
    for (auto& b : g->batches) {
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (!CreateBuffer(64ull << 20, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &b.ring) ||
            vkCreateDescriptorPool(g->vk.device, &dp, nullptr, &b.pool) != VK_SUCCESS ||
            vkCreateFence(g->vk.device, &fi, nullptr, &b.fence) != VK_SUCCESS) {
            if (error) *error = "ressources des lots (anneau, descripteurs, fence)";
            return false;
        }
        if (g->vk.timestampPeriod > 0) {
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = 2 + (GpuProfil() ? 1024 : 0);
            if (vkCreateQueryPool(g->vk.device, &qi, nullptr, &b.times) != VK_SUCCESS) b.times = VK_NULL_HANDLE;
        }
    }
    VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sc.magFilter = sc.minFilter = VK_FILTER_LINEAR;
    sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sc.maxLod = VK_LOD_CLAMP_NONE;
    if (config::g.anisotropy > 1 && g->vk.maxAnisotropy >= 1) {   // option filtrage_anisotrope
        sc.anisotropyEnable = VK_TRUE;
        sc.maxAnisotropy = std::min(float(config::g.anisotropy), g->vk.maxAnisotropy);
        fprintf(stderr, "[rendu] filtrage anisotrope %gx\n", sc.maxAnisotropy);
    }
    vkCreateSampler(g->vk.device, &sc, nullptr, &g->sampler);
    LoadPipelineCache();
    g->vk.CreateImage(1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &g->dummy);
    g->vk.Submit([&](VkCommandBuffer c) {
        Context::Transition(c, g->dummy.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        VkClearColorValue m{};
        m.float32[0] = 1.0f; m.float32[2] = 1.0f; m.float32[3] = 1.0f;   // magenta : texture absente
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(c, g->dummy.image, VK_IMAGE_LAYOUT_GENERAL, &m, 1, &r);
    });
    fprintf(stderr, "[rendu] Vulkan : %s ; mémoire physique invitée %s (512 Mo)\n", g->vk.deviceName.c_str(),
            exported ? "allouée par Vulkan et projetée" : "importée");
    return g->ok = true;
}

bool RendererActive() { return g && g->ok; }
void RendererSavePipelineCache() {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (RendererActive()) SavePipelineCache();
}
vk::Context* RendererContext() { return RendererActive() ? &g->vk : nullptr; }

void RendererDraw(const uint32_t* regs, uint64_t vsHash, uint64_t psHash, const DrawPacket& d) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!RendererActive()) return;
    if (!Cur().pendingWrites.empty() || !g->inFlight.empty()) {   // textures décodées sur CPU : attendre les resolves GPU qu'elles lisent
        bool overlap = false;
        for (uint32_t slot = 0; slot < 32 && !overlap; slot++) {
            const TextureInfo t = ParseTextureFetch(regs + REG_FETCH + slot * 6);
            if (t.type != 2 || !t.address || GpuDecodable(t)) continue;   // décodée sur GPU : dans l'ordre du flux
            const uint32_t a = t.address < kPhysSize ? kPhysBase | t.address : t.address;
            const uint64_t span = TextureSourceSpan(t);   // plage exacte ; sinon majorée (16 octets par texel)
            const uint32_t e = uint32_t(std::min<uint64_t>(0xFFFFFFFFull, a + (span ? span : uint64_t(std::max(t.pitch, t.width)) * t.height * 16)));
            for (auto& b : g->batches)
                for (auto& [b0, b1] : b.pendingWrites) if (a < b1 && b0 < e) { overlap = true; break; }
            if (overlap) {   // une ligne par format : texture lue sur CPU juste après un resolve (attente du GPU)
                static std::set<uint32_t> told;
                if (told.insert(t.format).second)
                    fprintf(stderr, "[rendu] attente de resolve pour une texture CPU : format %u (%ux%u)\n", t.format, t.width, t.height);
            }
        }
        if (overlap) { g->waitCause = &g->stats.waitsOverlap; Flush(); g->waitCause = nullptr; }
    }
    const uint32_t prim = d.initiator & 0x3F, count = d.initiator >> 16, source = (d.initiator >> 6) & 3;
    const bool index32 = (d.initiator >> 11) & 1;
    if (!count) return;
    VkPrimitiveTopology topo;
    switch (prim) {
        case 1: topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
        case 2: topo = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
        case 3: topo = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
        case 4: case 8: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;   // 8 (rectangles) : 4e sommet par géométrie
        case 5: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
        case 6: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
        case 13: topo = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;          // quadrilatères -> triangles (indices)
        default: g->stats.skipped++; return;
    }
    reg::RB_SURFACE_INFO si;
    si.value = regs[0x2000];
    uint32_t msx, msy;
    MsaaScale(si.value, &msx, &msy);
    Image* rt = Target(regs[0x2001], si.surface_pitch * msx);
    if (!rt) { g->stats.skipped++; return; }
    const uint32_t depthCtl = regs[0x2200];
    Image* ds = (depthCtl & 3) ? DepthTarget(regs[0x2002], si.surface_pitch * msx) : nullptr;   // stencil ou Z actif
    VkPipeline pipe = Pipeline(vsHash, psHash, topo, regs[0x2201], regs[0x2104] & 0xF, depthCtl, ds != nullptr, prim == 8);
    if (!pipe) { g->stats.skipped++; return; }
    // jeux : draw + un décodage GPU par emplacement de texture
    EnsureSpace(512 * 16 + 32 + 128 + 48 * 16, 1 + 32);
    VkCommandBuffer cmd = CmdForDraw();
    State::Batch& batch = Cur();

    // Descripteurs : constantes (copie des registres), mémoire, constantes de lecture, textures
    const VkDeviceSize constSize = 512 * 16 + 32 + 128, fetchSize = 48 * 16;
    // Copie seulement si un registre de constantes a changé depuis la dernière copie de ce lot (g_cpConstGen)
    VkDeviceSize co = batch.constOff, fo = batch.fetchOff;
    const bool copyConsts = batch.constGen != g_cpConstGen;
    if (copyConsts) {
        co = RingAlloc(constSize, g->vk.uniformAlignment);
        fo = RingAlloc(fetchSize, g->vk.uniformAlignment);
        if (co == UINT64_MAX || fo == UINT64_MAX) { g->stats.skipped++; return; }   // impossible après EnsureSpace
        memcpy(batch.ring.map + co, regs + REG_CONSTS, 512 * 16);
        memcpy(batch.ring.map + co + 512 * 16, regs + REG_BOOL, 32);
        memcpy(batch.ring.map + co + 512 * 16 + 32, regs + REG_LOOP, 128);
        batch.constGen = g_cpConstGen;
        batch.constOff = co;
        batch.fetchOff = fo;
        memcpy(batch.ring.map + fo, regs + REG_FETCH, fetchSize);
    }
    // Indices lus par le vertex shader dans la mémoire invitée (indexByte, indexMode) ; quadrilatères -> deux triangles
    // par la table de coins du shader. Rien n'est converti ni copié ici.
    uint32_t indexByte = 0, indexMode = 0, vertexCount = count;
    if (source == 0 && d.dma) {
        indexByte = (d.indexBase & ~1u) & (kPhysSize - 1);
        indexMode = (index32 ? 2u : 1u) | ((d.indexInfo >> 30) << 4);
    }
    if (prim == 13) { indexMode |= 0x100u; vertexCount = count / 4 * 6; }

    VkDescriptorImageInfo images[32];
    std::vector<uint32_t> vsTex, psTex;
    ShaderCacheGetSpirv(vsHash, nullptr, &vsTex);
    ShaderCacheGetSpirv(psHash, nullptr, &psTex);
    for (uint32_t i = 0; i < 32; i++) images[i] = {g->sampler, g->dummy.view, VK_IMAGE_LAYOUT_GENERAL};
    for (auto* list : {&vsTex, &psTex})
        for (uint32_t slot : *list) images[slot & 31].imageView = TextureView(cmd, regs, slot & 31);
    // Constantes par offsets dynamiques : le jeu ne dépend que des textures, réutilisé tant qu'elles ne changent pas
    VkDescriptorSet set = batch.lastSet;
    bool same = set != VK_NULL_HANDLE;
    for (uint32_t i = 0; i < 32 && same; i++) same = batch.lastViews[i] == images[i].imageView;
    if (!same) {
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        da.descriptorPool = batch.pool;
        da.descriptorSetCount = 1;
        da.pSetLayouts = &g->setLayout;
        if (vkAllocateDescriptorSets(g->vk.device, &da, &set) != VK_SUCCESS) { g->stats.skipped++; return; }
        batch.sets++;
        VkDescriptorBufferInfo bConst{batch.ring.buffer, 0, constSize}, bFetch{batch.ring.buffer, 0, fetchSize};
        VkDescriptorBufferInfo bMem[4];
        for (int k = 0; k < 4; k++) bMem[k] = {g->shared, VkDeviceSize(k) * (kPhysSize / 4), kPhysSize / 4};
        VkWriteDescriptorSet w[4] = {};
        for (int i = 0; i < 4; i++) { w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = set; w[i].dstBinding = uint32_t(i); w[i].descriptorCount = 1; }
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w[0].pBufferInfo = &bConst;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].descriptorCount = 4; w[1].pBufferInfo = bMem;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; w[2].pBufferInfo = &bFetch;
        w[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[3].descriptorCount = 32; w[3].pImageInfo = images;
        vkUpdateDescriptorSets(g->vk.device, 4, w, 0, nullptr);
        batch.lastSet = set;
        for (uint32_t i = 0; i < 32; i++) batch.lastViews[i] = images[i].imageView;
    }


    // Fenêtre : transformations VTE actives -> viewport Xenos ; sinon coordonnées écran -> NDC
    reg::PA_CL_VTE_CNTL vte;
    vte.value = regs[0x2206];
    float fv[6];
    memcpy(fv, regs + 0x210F, sizeof fv);
    int32_t sx0, sy0, sx1, sy1, vwx = 0, vwy = 0;
    WindowScissor(regs, &sx0, &sy0, &sx1, &sy1);
    if ((regs[0x2205] >> 16) & 1) WindowOffset(regs, &vwx, &vwy);   // vtx_window_offset_enable
    struct { float scale[4]; float offset[4]; int32_t indexOffset; uint32_t vte; uint32_t alphaFunc; float alphaRef; uint32_t indexByte, indexMode; } push{};
    VkViewport vp{};
    if (vte.vport_x_scale_ena && vte.vport_y_scale_ena) {
        const float xs = fv[0], xo = vte.vport_x_offset_ena ? fv[1] : 0, ys = fv[2], yo = vte.vport_y_offset_ena ? fv[3] : 0;
        const float zs = vte.vport_z_scale_ena ? fv[4] : 1, zo = vte.vport_z_offset_ena ? fv[5] : 0;
        vp = {(xo - xs + vwx) * msx, (yo - ys + vwy) * msy, 2 * xs * msx, 2 * ys * msy, zo, zo + zs};
        push.scale[0] = push.scale[1] = push.scale[2] = 1;
    } else {
        // sommets déjà en pixels (de la surface, éventuellement MSAA) : NDC = x * 2 / largeur - 1
        const float w = float(rt->width / msx), h = float(rt->height / msy);
        vp = {0, 0, float(rt->width), float(rt->height), 0, 1};
        push.scale[0] = 2 / w; push.scale[1] = 2 / h; push.scale[2] = 1;
        push.offset[0] = -1 + vwx * push.scale[0]; push.offset[1] = -1 + vwy * push.scale[1];
    }
    if (vp.width == 0 || vp.height == 0 || !std::isfinite(vp.width) || !std::isfinite(vp.height)) { g->stats.skipped++; return; }
    if (vp.minDepth < 0) vp.minDepth = 0;
    if (vp.maxDepth > 1) vp.maxDepth = 1;
    if (vp.minDepth > vp.maxDepth) std::swap(vp.minDepth, vp.maxDepth);
    push.indexOffset = int32_t(regs[0x2102]);
    push.indexByte = indexByte;
    push.indexMode = indexMode;
    push.vte = regs[0x2206];
    reg::RB_COLORCONTROL ctl;
    ctl.value = regs[0x2202];
    push.alphaFunc = ctl.alpha_test_enable ? uint32_t(ctl.alpha_func) : 7u;
    memcpy(&push.alphaRef, &regs[0x210E], 4);
    VkRect2D scissor{{sx0 * int32_t(msx), sy0 * int32_t(msy)},
                     {uint32_t(sx1 > sx0 ? sx1 - sx0 : 0) * msx, uint32_t(sy1 > sy0 ? sy1 - sy0 : 0) * msy}};
    scissor.offset.x = std::min(scissor.offset.x, int32_t(rt->width));
    scissor.offset.y = std::min(scissor.offset.y, int32_t(rt->height));
    scissor.extent.width = std::min(scissor.extent.width, rt->width - uint32_t(scissor.offset.x));
    scissor.extent.height = std::min(scissor.extent.height, rt->height - uint32_t(scissor.offset.y));

    VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = rt->view;
    att.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {rt->width, rt->height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    VkRenderingAttachmentInfo datt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (ds) {
        datt.imageView = ds->view;
        datt.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        datt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        datt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ri.pDepthAttachment = ri.pStencilAttachment = &datt;
    }
    // Passe gardée ouverte tant que cible et profondeur sont les mêmes (attachements : ordre de rastérisation garanti)
    const VkImage dsImage = ds ? ds->image : VK_NULL_HANDLE;
    auto pc = g->pendingClears.find(rt->image), pd = ds ? g->pendingClears.find(dsImage) : g->pendingClears.end();
    if (pc != g->pendingClears.end()) { att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att.clearValue = pc->second.value; }
    if (pd != g->pendingClears.end()) { datt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; datt.clearValue = pd->second.value; }
    const bool clearing = pc != g->pendingClears.end() || pd != g->pendingClears.end();
    if (!g->passOpen || g->passRt != rt->image || g->passDs != dsImage || clearing) {
        if (pc != g->pendingClears.end()) g->pendingClears.erase(pc);
        if (ds && (pd = g->pendingClears.find(dsImage)) != g->pendingClears.end()) g->pendingClears.erase(pd);
        ClosePass();
        vkCmdBeginRendering(cmd, &ri);
        g->passOpen = true;
        g->passRt = rt->image;
        g->passDs = dsImage;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    if (ds) {
        reg::RB_STENCILREFMASK sm;
        sm.value = regs[0x210D];
        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, sm.stencilref);
        vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, sm.stencilmask);
        vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, sm.stencilwritemask);
    }
    const uint32_t dyn[2] = {uint32_t(co), uint32_t(fo)};   // bindings 0 (constantes) et 2 (constantes de lecture)
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g->layout, 0, 1, &set, 2, dyn);
    vkCmdPushConstants(cmd, g->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, &push);
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdDraw(cmd, vertexCount, 1, 0, 0);
    g->stats.drawn++;
}

// Resolve sur GPU : un calcul lit la cible (texelFetch) et écrit les texels tuilés, au mode d'octets voulu,
// directement dans la mémoire physique invitée (tampon partagé). Pas d'attente CPU : les lecteurs CPU (décodage
// de textures) attendent la fin du lot s'ils touchent une plage en attente (cf. RendererDraw).
const char* kResolveGlsl = R"(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(std430, set = 0, binding = 1) buffer Mem { uint w[]; } mem;
layout(push_constant) uniform P { uint x0, y0, w, h, pitch, endian, swp, baseWord, msx, msy, depth; } p;
uint tiledOuter(uint y, uint width, uint l) {
    uint macro = ((y >> 5) * (width >> 5)) << (l + 7);
    uint micro = ((y & 6u) << 2) << l;
    return macro + ((micro & ~0xFu) << 1) + (micro & 0xFu) + ((y & 8u) << (3 + l)) + ((y & 1u) << 4);
}
uint tiled(uint x, uint y, uint width, uint l) {
    uint macro = (x >> 5) << (l + 7);
    uint micro = (x & 7u) << l;
    uint off = tiledOuter(y, width, l) + macro + ((micro & ~0xFu) << 1) + (micro & 0xFu);
    return ((off & ~0x1FFu) << 3) + ((off & 0x1C0u) << 2) + (off & 0x3Fu) + ((y & 16u) << 7) +
           (((((y & 8u) >> 2) + (x >> 3)) & 3u) << 6);
}
uint endianSwap(uint v, uint e) {
    if (e == 1u) return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    if (e == 2u) return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    if (e == 3u) return (v << 16) | (v >> 16);
    return v;
}
void main() {
    uvec2 g = gl_GlobalInvocationID.xy;
    if (g.x >= p.w || g.y >= p.h) return;
    ivec2 s0 = ivec2((p.x0 + g.x) * p.msx, (p.y0 + g.y) * p.msy);
    vec4 c = texelFetch(src, s0, 0);
    if (p.depth == 0u && p.msx * p.msy > 1u) {   // MSAA : moyenne des échantillons du pixel
        c = vec4(0.0);
        for (uint sy = 0u; sy < p.msy; sy++)
            for (uint sx = 0u; sx < p.msx; sx++) c += texelFetch(src, s0 + ivec2(sx, sy), 0);
        c /= float(p.msx * p.msy);
    }
    uvec4 b = uvec4(round(clamp(c, 0.0, 1.0) * 255.0));
    uint v = p.swp != 0u ? (b.z | (b.y << 8) | (b.x << 16) | (b.w << 24)) : (b.x | (b.y << 8) | (b.z << 16) | (b.w << 24));
    if (p.depth != 0u) v = uint(round(clamp(c.r, 0.0, 1.0) * 16777215.0)) << 8;   // D24S8 : profondeur haute, stencil 0
    mem.w[p.baseWord + (tiled(g.x, g.y, p.pitch, 2u) >> 2)] = endianSwap(v, p.endian);
}
)";

bool ResolvePipeline() {
    if (g->resolveTried) return g->resolvePipe != VK_NULL_HANDLE;
    g->resolveTried = true;
    std::vector<uint32_t> spv;
    std::string log;
    if (!CompileGlsl(2, kResolveGlsl, &spv, &log)) { fprintf(stderr, "[rendu] resolve GPU indisponible : %s\n", log.c_str()); return false; }
    VkDescriptorSetLayoutBinding b[2] = {};
    b[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 2;
    dl.pBindings = b;
    vkCreateDescriptorSetLayout(g->vk.device, &dl, nullptr, &g->resolveSetLayout);
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 44};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g->resolveSetLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pr;
    vkCreatePipelineLayout(g->vk.device, &pl, nullptr, &g->resolveLayout);
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = spv.size() * 4;
    mi.pCode = spv.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    vkCreateShaderModule(g->vk.device, &mi, nullptr, &mod);
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr};
    ci.layout = g->resolveLayout;
    if (vkCreateComputePipelines(g->vk.device, VK_NULL_HANDLE, 1, &ci, nullptr, &g->resolvePipe) != VK_SUCCESS) g->resolvePipe = VK_NULL_HANDLE;
    vkDestroyShaderModule(g->vk.device, mod, nullptr);
    return g->resolvePipe != VK_NULL_HANDLE;
}

// Resolve 8888 sur GPU ; false si impossible (repli CPU).
bool ResolveOnGpu(Image* rt, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t base, uint32_t pitch,
                  uint32_t endian, uint32_t swap, uint32_t msx, uint32_t msy, VkImageView depthView = VK_NULL_HANDLE) {
    if (base < kPhysBase || base >= kPhysBase + kPhysSize || !ResolvePipeline()) return false;
    ApplyPendingClear(rt->image);   // la copie lit la cible après son effacement en attente
    const VkDeviceSize rel = base - kPhysBase, off = rel & ~VkDeviceSize(255);
    const VkDeviceSize need = (rel - off) + VkDeviceSize((y1 - y0 + 31) & ~31u) * pitch * 4 + 4096;
    if (off + need > kPhysSize) return false;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    EnsureSpace(0, 1);
    ai.descriptorPool = Cur().pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &g->resolveSetLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(g->vk.device, &ai, &set) != VK_SUCCESS) return false;
    Cur().sets++;
    VkDescriptorImageInfo ii{g->sampler, depthView ? depthView : rt->view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo bi{g->shared, off, need};
    VkWriteDescriptorSet w[2] = {};
    for (int i = 0; i < 2; i++) { w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = set; w[i].dstBinding = uint32_t(i); w[i].descriptorCount = 1; }
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[0].pImageInfo = &ii;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo = &bi;
    vkUpdateDescriptorSets(g->vk.device, 2, w, 0, nullptr);
    struct { uint32_t x0, y0, w, h, pitch, endian, swp, baseWord, msx, msy, depth; } pc{
        x0, y0, x1 - x0, y1 - y0, pitch, endian, swap, uint32_t((rel - off) / 4), msx, msy, depthView ? 1u : 0u};
    VkCommandBuffer cmd = Cmd();
    Barrier(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g->resolvePipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g->resolveLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, g->resolveLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cmd, (pc.w + 7) / 8, (pc.h + 7) / 8, 1);
    Barrier(cmd);
    GpuMark(3);
    Cur().pendingWrites.push_back({base, uint32_t(base + need)});
    platform::WatchTouch(base, uint32_t(base + need), false);   // textures décodées sur GPU depuis cette plage : à refaire
    return true;
}

void RendererResolve(const uint32_t* regs) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!RendererActive()) return;
    reg::RB_COPY_CONTROL cc;
    cc.value = regs[0x2318];
    reg::RB_COPY_DEST_INFO di;
    di.value = regs[0x231B];
    reg::RB_SURFACE_INFO si;
    si.value = regs[0x2000];
    const uint32_t src = cc.copy_src_select;
    uint32_t msx, msy;
    MsaaScale(si.value, &msx, &msy);
    auto clearDepth = [&] {   // après la copie : RB_DEPTH_CLEAR, profondeur 24 bits (haut) et stencil 8 bits (bas)
        if (!cc.depth_clear_enable) return;
        if (Image* ds = DepthTarget(regs[0x2002], si.surface_pitch * msx)) {
            const uint32_t v = regs[0x231D];
            VkClearValue z{};
            z.depthStencil = {float(v >> 8) / 16777215.0f, v & 0xFF};
            g->pendingClears[ds->image] = {z, true};
            g->stats.clears++;
        }
    };
    if (src > 3) {   // copie de la profondeur vers la mémoire (mots D24S8 tuilés), sur GPU
        Image* ds = DepthTarget(regs[0x2002], si.surface_pitch * msx);
        auto dv = ds ? g->depthSampleViews.find(ds->image) : g->depthSampleViews.end();
        uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        ResolveRect(regs, &x0, &y0, &x1, &y1);
        const uint32_t destBase = regs[0x2319], destPitch = regs[0x231A] & 0x3FFF;
        if (ds) { x1 = std::min(x1, ds->width / msx); y1 = std::min(y1, ds->height / msy); }
        const uint32_t gbase = destBase < kPhysSize ? kPhysBase | destBase : destBase;
        if (ds && dv != g->depthSampleViews.end() && uint32_t(cc.copy_command) != 3 && destBase && destPitch && x1 > x0 && y1 > y0 &&
            ResolveOnGpu(ds, x0, y0, x1, y1, gbase, (destPitch + 31) & ~31u, uint32_t(di.copy_dest_endian), 0, msx, msy, dv->second))
            g->stats.resolves++;
        else
            g->stats.resolvesSkipped++;
        clearDepth();
        return;
    }
    static const uint32_t kColorInfo[4] = {0x2001, 0x2003, 0x2004, 0x2005};
    Image* rt = Target(regs[kColorInfo[src]], si.surface_pitch * msx);
    if (!rt) { g->stats.resolvesSkipped++; return; }
    uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    ResolveRect(regs, &x0, &y0, &x1, &y1);
    x1 = std::min(x1, rt->width / msx); y1 = std::min(y1, rt->height / msy);
    const uint32_t destBase = regs[0x2319], destPitch = regs[0x231A] & 0x3FFF;
    const uint32_t fmt = uint32_t(di.copy_dest_format);
    // Copie (si demandée) : relecture de la cible puis écriture tuilée en mémoire invitée
    if (uint32_t(cc.copy_command) != 3 && destBase && destPitch && x1 > x0 && y1 > y0) {
        const uint32_t gbase = destBase < kPhysSize ? kPhysBase | destBase : destBase;
        if (fmt != 6) {
            g->stats.resolvesSkipped++;
            static std::set<uint32_t> reported;   // une ligne par format de destination
            if (reported.insert(fmt).second)
                fprintf(stderr, "[rendu] resolve non géré : format de destination %u (cible %u, %ux%u)\n", fmt,
                        uint32_t((regs[kColorInfo[src]] >> 16) & 0xF), x1 - x0, y1 - y0);
        } else if (!getenv("WAKFU_CPU_RESOLVE") &&
                   ResolveOnGpu(rt, x0, y0, x1, y1, gbase, (destPitch + 31) & ~31u, uint32_t(di.copy_dest_endian),
                                di.copy_dest_swap, msx, msy)) {
            g->stats.resolves++;
        } else {
            static bool told = false;   // une ligne : resolve repassé sur CPU (attente du GPU)
            if (!told) { told = true; fprintf(stderr, "[rendu] resolve sur CPU : destination %08X pas %u (%ux%u)\n", gbase, destPitch, x1 - x0, y1 - y0); }
            g->waitCause = &g->stats.waitsOverlap;
            Flush();
            g->waitCause = nullptr;
            std::vector<uint8_t> px;
            ApplyPendingClear(rt->image);
            Flush();
            g->vk.Readback(*rt, VK_IMAGE_LAYOUT_GENERAL, &px);
            const uint32_t base = destBase < kPhysSize ? kPhysBase | destBase : destBase, endian = uint32_t(di.copy_dest_endian);
            const uint32_t pitch = (destPitch + 31) & ~31u;
            for (uint32_t y = y0; y < y1; y++)
                for (uint32_t x = x0; x < x1; x++) {
                    const uint8_t* p = &px[(size_t(y) * msy * rt->width + x * msx) * 4];
                    uint32_t v = di.copy_dest_swap ? (uint32_t(p[2]) | uint32_t(p[1]) << 8 | uint32_t(p[0]) << 16 | uint32_t(p[3]) << 24)
                                                   : (uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24);
                    const uint32_t off = TiledOffset2D(x - x0, y - y0, pitch, 2);
                    St32(base + off, __builtin_bswap32(GpuEndianSwap(v, endian)));
                }
            g->stats.resolves++;
        }
    }
    clearDepth();
    // Effacement de la cible après copie
    if (cc.color_clear_enable) {
        const uint32_t c = regs[0x231E];
        VkClearValue col{};
        col.color.float32[0] = float((c >> 16) & 0xFF) / 255.0f;   // RB_COLOR_CLEAR : ARGB
        col.color.float32[1] = float((c >> 8) & 0xFF) / 255.0f;
        col.color.float32[2] = float(c & 0xFF) / 255.0f;
        col.color.float32[3] = float(c >> 24) / 255.0f;
        g->pendingClears[rt->image] = {col, false};
        g->stats.clears++;
    }
}

void RendererSyncCpu() {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (RendererActive()) { g->waitCause = &g->stats.waitsSync; Flush(); g->waitCause = nullptr; }   // tout le travail GPU enregistré doit être fini
}

bool RendererDefer(std::function<void()> op) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!RendererActive()) return false;
    while (RetireOldest(false)) {}
    if (g->cmd) {                       // travail enregistré : l'écriture attend sa fin ; le lot part tout de suite
        Cur().ops.push_back(std::move(op));
        Submit();
        return true;
    }
    if (g->inFlight.empty()) return false;   // GPU au repos : écriture immédiate
    g->batches[g->inFlight.back()].ops.push_back(std::move(op));
    return true;
}

bool RendererWait(uint64_t timeoutNs) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!RendererActive() || g->inFlight.empty()) return false;
    vkWaitForFences(g->vk.device, 1, &g->batches[g->inFlight.front()].fence, VK_TRUE, timeoutNs);
    while (RetireOldest(false)) {}
    return true;
}

RendererFrameStats RendererEndFrame() {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!RendererActive()) return {};
    Submit();   // sans attente : la présentation est différée à la fin du lot (RendererDefer)
    g->frame++;
    RendererFrameStats s = g->stats;
    g->stats = RendererFrameStats{};
    return s;
}

}  // namespace wk::gpu
