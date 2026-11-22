// Décodage des textures sur GPU (kTexDecodeGlsl) comparé au décodage CPU (DecodeTexture / DecodeDepthTexture) :
// chaque format, tuilé ou non, chaque mode d'octets, plusieurs swizzles et tailles, données aléatoires. Sans écran
// (pilote choisi par VK_ICD_FILENAMES). Sortie identique octet par octet (profondeur : flottants identiques).
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#include "../src/gpu/glsl_translator.h"
#include "../src/gpu/texture_decode.h"
#include "../src/gpu/vk.h"
#include "../src/guest.h"

namespace wk { uint8_t* g_base = nullptr; }
using namespace wk::vk;
using namespace wk::gpu;

namespace {
constexpr uint32_t kMemSize = 32u << 20, kPhysBase = 0xA0000000u;
Context c;

bool Pipeline(bool depth, VkPipelineLayout layout, VkPipeline* pipe) {
    std::vector<uint32_t> spv;
    std::string log;
    const std::string src = std::string("#version 450\n") + (depth ? "#define DEPTH\n" : "") + kTexDecodeGlsl;
    if (!CompileGlsl(2, src, &spv, &log)) { printf("ÉCHEC compilation : %s\n", log.c_str()); return false; }
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = spv.size() * 4;
    mi.pCode = spv.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    vkCreateShaderModule(c.device, &mi, nullptr, &mod);
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr};
    ci.layout = layout;
    const bool ok = vkCreateComputePipelines(c.device, VK_NULL_HANDLE, 1, &ci, nullptr, pipe) == VK_SUCCESS;
    vkDestroyShaderModule(c.device, mod, nullptr);
    return ok;
}

struct HostBuffer { VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; uint8_t* map = nullptr; };
bool CreateHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage, HostBuffer* b) {
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    if (vkCreateBuffer(c.device, &ci, nullptr, &b->buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(c.device, b->buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = c.FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(c.device, &ai, nullptr, &b->memory) != VK_SUCCESS) return false;
    vkBindBufferMemory(c.device, b->buffer, b->memory, 0);
    void* p;
    vkMapMemory(c.device, b->memory, 0, size, 0, &p);
    b->map = static_cast<uint8_t*>(p);
    return true;
}
}  // namespace

int main() {
    std::string err;
    if (!c.Init(&err)) { printf("ÉCHEC init : %s\n", err.c_str()); return 1; }
    printf("GPU : %s\n", c.deviceName.c_str());

    VkDescriptorSetLayoutBinding b[2] = {};
    b[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 2;
    dl.pBindings = b;
    VkDescriptorSetLayout setLayout;
    vkCreateDescriptorSetLayout(c.device, &dl, nullptr, &setLayout);
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TexDecodeParams)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &setLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pr;
    VkPipelineLayout layout;
    vkCreatePipelineLayout(c.device, &pl, nullptr, &layout);
    VkPipeline pipes[2];
    if (!Pipeline(false, layout, &pipes[0]) || !Pipeline(true, layout, &pipes[1])) return 1;
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 1;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = ps;
    VkDescriptorPool pool;
    vkCreateDescriptorPool(c.device, &dp, nullptr, &pool);

    // Mémoire invitée : même contenu aléatoire côté CPU (Host(0xA0000000 + x)) et dans le tampon lu par le GPU
    HostBuffer mem, out;
    if (!CreateHostBuffer(kMemSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &mem) ||
        !CreateHostBuffer(1280 * 720 * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &out)) { printf("ÉCHEC tampons\n"); return 1; }
    std::vector<uint8_t> host(kMemSize);
    std::mt19937 rng(12345);
    for (auto& v : host) v = uint8_t(rng());
    memcpy(mem.map, host.data(), kMemSize);
    wk::g_base = host.data() - kPhysBase;   // Host(0xA0000000 | adresse) -> host[adresse]

    const uint32_t formats[] = {2, 3, 4, 6, 10, 15, 18, 19, 20, 22, 23};
    // 1280x720 (pas 40 x 32) et 640x360 (pas 24 x 32) : plans Y/U/V k_8 linéaires de la vidéo de l'écran-titre
    struct Size { uint32_t w, h, pitchField; } sizes[] = {{37, 23, 2}, {64, 64, 0}, {128, 40, 5}, {5, 3, 1}, {1280, 720, 40}, {640, 360, 24}};
    const uint32_t swizzles[] = {0x688 /* xyzw */, 0xFAC /* 4,5,6,7 */, 0x0D1, 0x5A3};
    uint32_t cases = 0, failures = 0;
    for (uint32_t format : formats)
        for (uint32_t tiled = 0; tiled < 2; tiled++)
            for (uint32_t endian = 0; endian < 4; endian++)
                for (const Size& s : sizes)
                    for (uint32_t swz : swizzles) {
                        TextureInfo t;
                        t.type = 2;
                        t.dimension = 1;
                        t.format = format;
                        t.tiled = tiled;
                        t.endian = endian;
                        t.width = s.w;
                        t.height = s.h;
                        t.pitch = s.pitchField * 32;   // comme ParseTextureFetch (0 : largeur)
                        t.swizzle = swz;
                        t.address = 0x1000u * (1 + (rng() % 512));
                        const uint64_t span = TextureSourceSpan(t);
                        if (!span || t.address + span + 4 > kMemSize) { printf("ÉCHEC plage %u\n", format); return 1; }
                        const bool depth = format == 22 || format == 23;
                        // référence CPU
                        std::vector<uint8_t> ref;
                        std::string why;
                        if (depth) {
                            std::vector<float> d;
                            if (!DecodeDepthTexture(t, &d)) { printf("ÉCHEC CPU %u\n", format); return 1; }
                            ref.resize(d.size() * 16);
                            for (size_t i = 0; i < d.size(); i++)
                                for (int k = 0; k < 4; k++) {
                                    const uint32_t sel = (swz >> (3 * k)) & 7;   // vue CPU : x, y, z -> profondeur ; 0 ; sinon 1
                                    const float v = sel < 3 ? d[i] : sel == 4 ? 0.0f : 1.0f;
                                    memcpy(&ref[i * 16 + k * 4], &v, 4);
                                }
                        } else if (!DecodeTexture(t, &ref, &why)) { printf("ÉCHEC CPU %u : %s\n", format, why.c_str()); return 1; }
                        // GPU : liaison comme le rendu (tampon à partir de l'adresse, plage + 4 octets)
                        TexDecodeParams pc;
                        uint32_t gx, gy;
                        if (!TexDecodeSetup(t, 0, &pc, &gx, &gy)) { printf("ÉCHEC paramètres %u\n", format); return 1; }
                        Image img;
                        const VkFormat vf = depth ? VK_FORMAT_R32G32B32A32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
                        if (!c.CreateImage(t.width, t.height, vf, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &img)) {
                            printf("ÉCHEC image\n");
                            return 1;
                        }
                        vkResetDescriptorPool(c.device, pool, 0);
                        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                        ai.descriptorPool = pool;
                        ai.descriptorSetCount = 1;
                        ai.pSetLayouts = &setLayout;
                        VkDescriptorSet set;
                        vkAllocateDescriptorSets(c.device, &ai, &set);
                        VkDescriptorBufferInfo bi{mem.buffer, t.address, span + 4};
                        VkDescriptorImageInfo ii{VK_NULL_HANDLE, img.view, VK_IMAGE_LAYOUT_GENERAL};
                        VkWriteDescriptorSet w[2] = {};
                        for (int i = 0; i < 2; i++) { w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet = set; w[i].dstBinding = uint32_t(i); w[i].descriptorCount = 1; }
                        w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[0].pBufferInfo = &bi;
                        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[1].pImageInfo = &ii;
                        vkUpdateDescriptorSets(c.device, 2, w, 0, nullptr);
                        const uint32_t bpt = depth ? 16 : 4;
                        c.Submit([&](VkCommandBuffer cmd) {
                            Context::Transition(cmd, img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
                            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipes[depth]);
                            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
                            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
                            vkCmdDispatch(cmd, gx, gy, 1);
                            Context::Transition(cmd, img.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                            VkBufferImageCopy region{};
                            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                            region.imageExtent = {t.width, t.height, 1};
                            vkCmdCopyImageToBuffer(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out.buffer, 1, &region);
                        });
                        c.DestroyImage(img);
                        cases++;
                        const size_t n = size_t(t.width) * t.height * bpt;
                        if (ref.size() != n || memcmp(ref.data(), out.map, n) != 0) {
                            size_t i = 0;
                            while (i < ref.size() && i < n && ref[i] == out.map[i]) i++;
                            const size_t px = i / bpt;
                            if (failures++ < 20)
                                printf("DIFF format %u tuilé %u octets %u %ux%u pas %u swizzle %03X : texel (%zu, %zu) octet %zu CPU %u GPU %u\n",
                                       format, tiled, endian, t.width, t.height, t.pitch, swz, px % t.width, px / t.width, i % bpt,
                                       i < ref.size() ? ref[i] : 0, out.map[i]);
                        }
                    }
    vkDestroyDescriptorPool(c.device, pool, nullptr);
    for (VkPipeline p : pipes) vkDestroyPipeline(c.device, p, nullptr);
    vkDestroyPipelineLayout(c.device, layout, nullptr);
    vkDestroyDescriptorSetLayout(c.device, setLayout, nullptr);
    for (HostBuffer* hb : {&mem, &out}) { vkDestroyBuffer(c.device, hb->buffer, nullptr); vkFreeMemory(c.device, hb->memory, nullptr); }
    c.Shutdown();
    printf("%u cas, %u différences : %s\n", cases, failures, failures ? "ÉCHEC" : "OK");
    return failures ? 1 : 0;
}
