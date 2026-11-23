// Tampons natifs (cf. tampons.h).
#include "natif/tampons.h"
#include <bit>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include "guest.h"

namespace natif {
namespace {

struct Owned { VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; VkDeviceSize cap = 0; };
struct Entry {
    Owned dev;               // tampons statiques
    uint64_t key = 0;        // version convertie (flux, décalage, pas) ; 0 = rien
    bool dirty = true, dynamic = false;
    uint64_t lastUnlock = ~0ull, ringFrame = ~0ull;
    Buffer ring;             // dynamiques : copie de l'image ringFrame
};
struct Dead { Owned o; uint64_t due; };

wk::vk::Context* g_vk = nullptr;
uint32_t g_frames = 2;
VkDeviceSize g_ringPer = 0, g_ringUsed = 0;
Owned g_ring;
uint8_t* g_ringMap = nullptr;
uint64_t g_frame = 0;
std::mutex g_mu;
std::unordered_map<uint32_t, Entry> g_entries;
std::vector<Dead> g_dead;

bool Create(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, Owned* o) {
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    if (wk::vk::vkCreateBuffer(g_vk->device, &ci, nullptr, &o->buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    wk::vk::vkGetBufferMemoryRequirements(g_vk->device, o->buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = g_vk->FindMemory(req.memoryTypeBits, props);
    if (ai.memoryTypeIndex == UINT32_MAX || wk::vk::vkAllocateMemory(g_vk->device, &ai, nullptr, &o->memory) != VK_SUCCESS ||
        wk::vk::vkBindBufferMemory(g_vk->device, o->buffer, o->memory, 0) != VK_SUCCESS) {
        wk::vk::vkDestroyBuffer(g_vk->device, o->buffer, nullptr);
        if (o->memory) wk::vk::vkFreeMemory(g_vk->device, o->memory, nullptr);
        *o = {};
        return false;
    }
    o->cap = size;
    return true;
}
void Destroy(Owned& o) {
    if (o.buffer) wk::vk::vkDestroyBuffer(g_vk->device, o.buffer, nullptr);
    if (o.memory) wk::vk::vkFreeMemory(g_vk->device, o.memory, nullptr);
    o = {};
}

// Réserve `size` octets de l'anneau de l'image courante ; nullptr si plein.
uint8_t* RingAlloc(VkDeviceSize size, Buffer* out) {
    const VkDeviceSize at = (g_ringUsed + 15) & ~VkDeviceSize(15);
    if (!g_ringMap || at + size > g_ringPer) {
        static bool once = false;
        if (!once) fprintf(stderr, "[natif] anneau d'envoi plein (%llu o par image)\n", (unsigned long long)g_ringPer), once = true;
        return nullptr;
    }
    g_ringUsed = at + size;
    *out = {g_ring.buffer, (g_frame % g_frames) * g_ringPer + at, size};
    return g_ringMap + out->offset;
}

// Conversion `fill` vers l'anneau, puis tampon statique (copie) ou dynamique (anneau tel quel).
template <class F>
Buffer Serve(VkCommandBuffer upload, uint32_t res, uint64_t key, VkDeviceSize size, F fill) {
    std::lock_guard lk(g_mu);
    Entry& e = g_entries[res];
    if (e.dynamic) {
        if (e.key == key && e.ringFrame == g_frame && !e.dirty) return e.ring;
    } else if (e.key == key && !e.dirty) {
        return {e.dev.buffer, 0, size};
    }
    Buffer staged;
    uint8_t* p = RingAlloc(size, &staged);
    if (!p) return {};
    fill(p);
    e.key = key;
    e.dirty = false;
    if (e.dynamic) { e.ring = staged; e.ringFrame = g_frame; return staged; }
    if (e.dev.cap < size) {
        if (e.dev.buffer) g_dead.push_back({e.dev, g_frame + g_frames});
        e.dev = {};
        if (!Create(size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &e.dev)) { e.key = 0; return {}; }
    }
    // Les draws précédents de l'image lisent peut-être l'ancienne version : attente lecture -> écriture -> lecture.
    VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = e.dev.buffer;
    b.size = VK_WHOLE_SIZE;
    b.srcAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    wk::vk::vkCmdPipelineBarrier(upload, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &b, 0, nullptr);
    const VkBufferCopy c{staged.offset, 0, size};
    wk::vk::vkCmdCopyBuffer(upload, staged.buffer, e.dev.buffer, 1, &c);
    std::swap(b.srcAccessMask, b.dstAccessMask);
    wk::vk::vkCmdPipelineBarrier(upload, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, nullptr, 1, &b, 0, nullptr);
    return {e.dev.buffer, 0, size};
}

// Adresse physique (< 512 Mo, ex. sommets du lecteur vidéo XMV) : vue invitée 0xA0000000+, comme l'émulation
uint32_t Invitee(uint32_t a) { return a < 0x20000000u ? 0xA0000000u | a : a; }

}  // namespace

bool BuffersInit(wk::vk::Context* vk, uint32_t frames, VkDeviceSize ringBytesPerFrame) {
    if (!vk) return false;
    g_vk = vk;
    g_frames = frames ? frames : 1;
    g_ringPer = ringBytesPerFrame;
    if (!Create(g_ringPer * g_frames, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &g_ring))
        return false;
    void* p = nullptr;
    if (wk::vk::vkMapMemory(vk->device, g_ring.memory, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) { Destroy(g_ring); return false; }
    g_ringMap = (uint8_t*)p;
    return true;
}

void BuffersShutdown() {
    std::lock_guard lk(g_mu);
    if (!g_vk) return;
    for (auto& [res, e] : g_entries) Destroy(e.dev);
    for (Dead& d : g_dead) Destroy(d.o);
    g_entries.clear();
    g_dead.clear();
    Destroy(g_ring);   // libère aussi la projection
    g_ringMap = nullptr;
    g_vk = nullptr;
}

void BuffersBeginFrame(uint64_t frame) {
    std::lock_guard lk(g_mu);
    g_frame = frame;
    g_ringUsed = 0;
    for (size_t i = 0; i < g_dead.size();)
        if (g_dead[i].due <= frame) { Destroy(g_dead[i].o); g_dead[i] = g_dead.back(); g_dead.pop_back(); } else i++;
}

void OnUnlock(uint32_t guestRes) {
    std::lock_guard lk(g_mu);
    Entry& e = g_entries[guestRes];
    e.dirty = true;
    if (e.lastUnlock != ~0ull && g_frame - e.lastUnlock < 4 && !e.dynamic) {
        e.dynamic = true;   // définitif : un tampon réécrit souvent le reste
        if (e.dev.buffer) g_dead.push_back({e.dev, g_frame + g_frames});
        e.dev = {};
    }
    e.lastUnlock = g_frame;
}

void OnDestroy(uint32_t guestRes) {
    std::lock_guard lk(g_mu);
    auto it = g_entries.find(guestRes);
    if (it == g_entries.end()) return;
    if (it->second.dev.buffer) g_dead.push_back({it->second.dev, g_frame + g_frames});
    g_entries.erase(it);
}

Buffer BufferFor(VkCommandBuffer upload, uint32_t guestVb, const VertexLayout& l, uint32_t stream, uint32_t offset, uint32_t stride,
                 const float* pixels) {
    if (!guestVb || !stride || !g_vk) return {};
    const uint32_t addr = Invitee(wk::Ld32(guestVb + 24) & ~3u), size = wk::Ld32(guestVb + 28) & 0x03FFFFFC;
    if (offset >= size) return {};
    const uint32_t count = (size - offset) / stride, pcStride = PcStride(l, stream, stride);
    uint64_t key = StreamKey(l, stream) ^ (uint64_t(offset) << 32 | stride) * 0x9E3779B97F4A7C15ull;
    if (pixels) key ^= (uint64_t(std::bit_cast<uint32_t>(pixels[0])) << 32 | std::bit_cast<uint32_t>(pixels[1])) * 0xC2B2AE3D27D4EB4Full;
    return Serve(upload, guestVb, key ? key : 1, VkDeviceSize(count) * pcStride, [&](uint8_t* p) {
        ConvertVertices(l, stream, wk::Host(addr + offset), stride, count, p);
        if (pixels) PixelsVersNdc(l, stream, stride, count, pixels[0], pixels[1], p);
    });
}

Buffer RingUpload(VkDeviceSize size, uint8_t** dst) {
    std::lock_guard lk(g_mu);
    Buffer b;
    *dst = RingAlloc(size, &b);
    return *dst ? b : Buffer{};
}

Buffer IndexBufferFor(VkCommandBuffer upload, uint32_t guestIb, VkIndexType* type) {
    if (!guestIb || !g_vk) return {};
    const bool is32 = wk::Ld32(guestIb) & 0x80000000u;   // Common : Format << 29, INDEX16 = 1, INDEX32 = 6
    const uint32_t addr = Invitee(wk::Ld32(guestIb + 24)), size = wk::Ld32(guestIb + 28) & ~(is32 ? 3u : 1u);
    *type = is32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    if (!size) return {};
    return Serve(upload, guestIb, is32 ? 2 : 1, size, [&](uint8_t* p) { ConvertIndices(wk::Host(addr), size, is32, p); });
}

}  // namespace natif
