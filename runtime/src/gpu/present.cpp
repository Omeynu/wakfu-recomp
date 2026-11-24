// Présentation (voir present.h) : image native -> image source (blit) -> post-traitement / FSR -> étirement (blit)
// vers l'image de la chaîne d'échange, une image en vol.
#include "../config.h"
#include "present.h"
#include "fsr1.h"
#include "../png.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

namespace wk::gpu {

namespace {
using namespace wk::vk;
#define WK_PRESENT_INSTANCE(X)                                                                  \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)        \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define WK_PRESENT_DEVICE(X)                                                                    \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR)                 \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR)
#define WK_DEFINE(name) PFN_##name name = nullptr;
WK_PRESENT_INSTANCE(WK_DEFINE)
WK_PRESENT_DEVICE(WK_DEFINE)
#undef WK_DEFINE

struct State {
    vk::Context* ctx = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkSemaphore> done;               // une par image de la chaîne (présentation)
    VkSemaphore acquired = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool inFlight = false, stale = true;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vk::Image source;
    // Post-traitement (options [post]) : FXAA puis netteté / couleur, calculs GPU, image affichée seulement
    bool postTried = false, postOk = false;
    VkDescriptorSetLayout postSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout postLayout = VK_NULL_HANDLE;
    VkPipeline postPipe = VK_NULL_HANDLE;
    VkDescriptorPool postPool = VK_NULL_HANDLE;
    VkDescriptorSet postSets[2] = {};
    VkSampler postSampler = VK_NULL_HANDLE;
    vk::Image post[2];
    // Agrandissement AMD FSR 1 (option [post] agrandissement = "fsr1") : EASU (source -> fsr[0]) puis RCAS (-> fsr[1])
    bool fsrTried = false, fsrOk = false;
    VkDescriptorSetLayout fsrSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout fsrLayout = VK_NULL_HANDLE;
    VkPipeline fsrPipes[2] = {};
    VkDescriptorPool fsrPool = VK_NULL_HANDLE;
    VkDescriptorSet fsrSets[2] = {};
    VkSampler fsrSampler = VK_NULL_HANDLE;
    vk::Image fsr[2];
    // Message de capture : cadre RGBA copié sur l'image de la chaîne d'échange (tampon visible du CPU, fait une fois)
    VkBuffer texteBuf = VK_NULL_HANDLE;
    VkDeviceMemory texteMem = VK_NULL_HANDLE;
    uint32_t texteW = 0, texteH = 0, texteEchelle = 0;
};
State* g = nullptr;
std::atomic<int64_t> g_annonceFin{0};   // fin d'affichage du message de capture (steady_clock, ns)

int64_t MaintenantNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Cadre « Capture effectuée » (français) ou « Screenshot saved » à l'échelle `e`, dans un tampon visible du CPU.
bool EnsureTexte(uint32_t e) {
    if (g->texteBuf && g->texteEchelle == e) return true;
    VkDevice dev = g->ctx->device;
    if (g->texteBuf) vkDestroyBuffer(dev, g->texteBuf, nullptr), vkFreeMemory(dev, g->texteMem, nullptr);
    g->texteBuf = VK_NULL_HANDLE, g->texteMem = VK_NULL_HANDLE, g->texteEchelle = 0;
    const bool fr = config::LangueXbox(config::g.langue, config::LangueSysteme()) == 4;
    const std::vector<uint8_t> rgba = TexteRgba(fr ? "Capture effectu\xc3\xa9" "e" : "Screenshot saved", e, &g->texteW, &g->texteH);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = rgba.size();
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(dev, &bi, nullptr, &g->texteBuf) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, g->texteBuf, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = g->ctx->FindMemory(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    void* p = nullptr;
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &ai, nullptr, &g->texteMem) != VK_SUCCESS ||
        vkBindBufferMemory(dev, g->texteBuf, g->texteMem, 0) != VK_SUCCESS || vkMapMemory(dev, g->texteMem, 0, bi.size, 0, &p) != VK_SUCCESS) {
        if (g->texteMem) vkFreeMemory(dev, g->texteMem, nullptr);
        vkDestroyBuffer(dev, g->texteBuf, nullptr);
        g->texteBuf = VK_NULL_HANDLE, g->texteMem = VK_NULL_HANDLE;
        return false;
    }
    memcpy(p, rgba.data(), rgba.size());
    vkUnmapMemory(dev, g->texteMem);
    g->texteEchelle = e;
    return true;
}

// GLSL (calcul) -> SPIR-V (glslang)
bool CompileGlsl(const char* source, std::vector<uint32_t>* spirv, std::string* log) {
    // Statique locale (initialisation thread-safe) plutôt que std::call_once : sous MinGW, Clang (TLS natif) ne
    // trouve pas __once_call du libstdc++ de GCC (TLS émulé).
    static const bool initialized = glslang::InitializeProcess();
    (void)initialized;
    glslang::TShader shader(EShLangCompute);
    shader.setStrings(&source, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, EShLangCompute, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);
    if (!shader.parse(GetDefaultResources(), 450, false, EShMsgDefault)) {
        if (log) *log = shader.getInfoLog();
        return false;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(EShMsgDefault)) {
        if (log) *log = program.getInfoLog();
        return false;
    }
    glslang::GlslangToSpv(*program.getIntermediate(EShLangCompute), *spirv);
    return true;
}

// Passe 0 : FXAA (version « console » de T. Lottes). Passe 1 : netteté AMD CAS, correction couleur Xbox (courbe
// PWL du Xenos -> linéaire -> sRGB, approximation), luminosité, gamma.
const char* kPostGlsl = R"(#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1, rgba8) uniform writeonly image2D dst;
layout(push_constant) uniform P { uint pass; float sharp, bright, gamma; uint xbox; } p;
vec3 at(ivec2 c) { return texelFetch(src, clamp(c, ivec2(0), textureSize(src, 0) - 1), 0).rgb; }
vec3 pwlToLinear(vec3 c) {
    vec3 x = c * 255.0;
    vec3 l = mix(mix(x, 2.0 * x - 64.0, step(64.0, x)), mix(4.0 * x - 256.0, 8.0 * x - 1024.0, step(192.0, x)), step(96.0, x));
    return clamp(l / 1023.0, 0.0, 1.0);
}
vec3 linearToSrgb(vec3 l) { return mix(l * 12.92, 1.055 * pow(l, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, l)); }
void main() {
    ivec2 g = ivec2(gl_GlobalInvocationID.xy);
    ivec2 sz = textureSize(src, 0);
    if (any(greaterThanEqual(g, sz))) return;
    vec2 inv = 1.0 / vec2(sz), uv = (vec2(g) + 0.5) * inv;
    vec3 c;
    if (p.pass == 0u) {
        const vec3 W = vec3(0.299, 0.587, 0.114);
        vec3 m = texture(src, uv).rgb;
        float nw = dot(texture(src, uv + vec2(-1, -1) * inv).rgb, W), ne = dot(texture(src, uv + vec2(1, -1) * inv).rgb, W);
        float sw = dot(texture(src, uv + vec2(-1, 1) * inv).rgb, W), se = dot(texture(src, uv + vec2(1, 1) * inv).rgb, W);
        float lm = dot(m, W), lo = min(lm, min(min(nw, ne), min(sw, se))), hi = max(lm, max(max(nw, ne), max(sw, se)));
        vec2 dir = vec2(-((nw + ne) - (sw + se)), (nw + sw) - (ne + se));
        float red = max((nw + ne + sw + se) * (0.25 / 8.0), 1.0 / 128.0);
        float rcp = 1.0 / (min(abs(dir.x), abs(dir.y)) + red);
        dir = clamp(dir * rcp, vec2(-8.0), vec2(8.0)) * inv;
        vec3 a = 0.5 * (texture(src, uv + dir * (1.0 / 3.0 - 0.5)).rgb + texture(src, uv + dir * (2.0 / 3.0 - 0.5)).rgb);
        vec3 b = a * 0.5 + 0.25 * (texture(src, uv - dir * 0.5).rgb + texture(src, uv + dir * 0.5).rgb);
        float lb = dot(b, W);
        c = (lb < lo || lb > hi) ? a : b;
    } else {
        c = at(g);
        if (p.sharp > 0.0) {
            vec3 b = at(g + ivec2(0, -1)), d = at(g + ivec2(-1, 0)), f = at(g + ivec2(1, 0)), h = at(g + ivec2(0, 1));
            vec3 mn = min(min(min(b, d), min(f, h)), c), mx = max(max(max(b, d), max(f, h)), c);
            vec3 amp = sqrt(clamp(min(mn, 2.0 - mx) / max(mx, vec3(1e-5)), 0.0, 1.0));
            vec3 w = amp * mix(-0.125, -0.2, p.sharp);
            c = clamp((c + w * (b + d + f + h)) / (1.0 + 4.0 * w), 0.0, 1.0);
        }
        if (p.xbox != 0u) c = linearToSrgb(pwlToLinear(c));
        c = clamp(c * p.bright, 0.0, 1.0);
        if (p.gamma != 1.0) c = pow(c, vec3(1.0 / p.gamma));
    }
    imageStore(dst, g, vec4(c, 1.0));
}
)";

bool PostWanted() {
    const auto& o = config::g;
    return o.antiAliasing == "fxaa" || o.sharpness > 0 || o.brightness != 1.0f || o.gamma != 1.0f || o.xboxColor;
}
// Pipeline (une fois) et images de sortie à la taille de l'image source.
bool EnsurePost(uint32_t w, uint32_t h) {
    vk::Context& c = *g->ctx;
    if (!g->postTried) {
        g->postTried = true;
        std::vector<uint32_t> spv;
        std::string log;
        if (!CompileGlsl(kPostGlsl, &spv, &log)) { fprintf(stderr, "[fenetre] post-traitement indisponible : %s\n", log.c_str()); return false; }
        VkDescriptorSetLayoutBinding b[2] = {};
        b[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        b[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dl.bindingCount = 2;
        dl.pBindings = b;
        vkCreateDescriptorSetLayout(c.device, &dl, nullptr, &g->postSetLayout);
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 20};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &g->postSetLayout;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &pr;
        vkCreatePipelineLayout(c.device, &pl, nullptr, &g->postLayout);
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = spv.size() * 4;
        mi.pCode = spv.data();
        VkShaderModule mod = VK_NULL_HANDLE;
        vkCreateShaderModule(c.device, &mi, nullptr, &mod);
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr};
        ci.layout = g->postLayout;
        const bool ok = vkCreateComputePipelines(c.device, VK_NULL_HANDLE, 1, &ci, nullptr, &g->postPipe) == VK_SUCCESS;
        vkDestroyShaderModule(c.device, mod, nullptr);
        VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dp.maxSets = 2;
        dp.poolSizeCount = 2;
        dp.pPoolSizes = ps;
        vkCreateDescriptorPool(c.device, &dp, nullptr, &g->postPool);
        VkDescriptorSetLayout layouts[2] = {g->postSetLayout, g->postSetLayout};
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = g->postPool;
        ai.descriptorSetCount = 2;
        ai.pSetLayouts = layouts;
        VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sc.magFilter = sc.minFilter = VK_FILTER_LINEAR;
        sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        g->postOk = ok && vkAllocateDescriptorSets(c.device, &ai, g->postSets) == VK_SUCCESS &&
                    vkCreateSampler(c.device, &sc, nullptr, &g->postSampler) == VK_SUCCESS;
        const auto& o = config::g;
        fprintf(stderr, "[fenetre] post-traitement %s : anticrénelage %s, netteté %g, luminosité %g, gamma %g, couleur Xbox %s\n",
                g->postOk ? "actif" : "indisponible", o.antiAliasing.c_str(), o.sharpness, o.brightness, o.gamma, o.xboxColor ? "oui" : "non");
    }
    if (!g->postOk) return false;
    if (g->post[0].width != w || g->post[0].height != h) {
        for (auto& im : g->post) {
            if (im.image) c.DestroyImage(im);
            if (!c.CreateImage(w, h, VK_FORMAT_R8G8B8A8_UNORM,
                               VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &im))
                return g->postOk = false;
        }
        // jeu 0 : source -> post[0] (FXAA) ; jeu 1 : (post[0] si FXAA, sinon source) -> post[1]
        const bool fxaa = config::g.antiAliasing == "fxaa";
        VkDescriptorImageInfo in[2] = {{g->postSampler, g->source.view, VK_IMAGE_LAYOUT_GENERAL},
                                       {g->postSampler, fxaa ? g->post[0].view : g->source.view, VK_IMAGE_LAYOUT_GENERAL}};
        VkDescriptorImageInfo out[2] = {{VK_NULL_HANDLE, g->post[0].view, VK_IMAGE_LAYOUT_GENERAL},
                                        {VK_NULL_HANDLE, g->post[1].view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet wr[4] = {};
        for (int i = 0; i < 4; i++) {
            wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr[i].dstSet = g->postSets[i / 2];
            wr[i].dstBinding = uint32_t(i % 2);
            wr[i].descriptorCount = 1;
            wr[i].descriptorType = i % 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wr[i].pImageInfo = i % 2 ? &out[i / 2] : &in[i / 2];
        }
        vkUpdateDescriptorSets(c.device, 4, wr, 0, nullptr);
        g->ctx->Submit([&](VkCommandBuffer cmd) {
            for (auto& im : g->post) vk::Context::Transition(cmd, im.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        });
    }
    return true;
}

// Ressources de FSR 1 ; images de sortie à la taille affichée (outW x outH). Faux si indisponible (étirement linéaire).
bool EnsureFsr(uint32_t outW, uint32_t outH) {
    vk::Context& c = *g->ctx;
    if (!g->fsrTried) {
        g->fsrTried = true;
        VkDescriptorSetLayoutBinding b[2] = {};
        b[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        b[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dl.bindingCount = 2;
        dl.pBindings = b;
        vkCreateDescriptorSetLayout(c.device, &dl, nullptr, &g->fsrSetLayout);
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 64};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &g->fsrSetLayout;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &pr;
        vkCreatePipelineLayout(c.device, &pl, nullptr, &g->fsrLayout);
        bool ok = true;
        for (int i = 0; i < 2 && ok; i++) {
            std::vector<uint32_t> spv;
            std::string log;
            if (!CompileGlsl(Fsr1Glsl(i == 0).c_str(), &spv, &log)) {
                fprintf(stderr, "[fenetre] FSR 1 indisponible : %s\n", log.substr(0, 300).c_str());
                ok = false;
                break;
            }
            VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            mi.codeSize = spv.size() * 4;
            mi.pCode = spv.data();
            VkShaderModule mod = VK_NULL_HANDLE;
            vkCreateShaderModule(c.device, &mi, nullptr, &mod);
            VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr};
            ci.layout = g->fsrLayout;
            ok = vkCreateComputePipelines(c.device, VK_NULL_HANDLE, 1, &ci, nullptr, &g->fsrPipes[i]) == VK_SUCCESS;
            vkDestroyShaderModule(c.device, mod, nullptr);
        }
        VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dp.maxSets = 2;
        dp.poolSizeCount = 2;
        dp.pPoolSizes = ps;
        VkDescriptorSetLayout layouts[2] = {g->fsrSetLayout, g->fsrSetLayout};
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorSetCount = 2;
        ai.pSetLayouts = layouts;
        VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sc.magFilter = sc.minFilter = VK_FILTER_LINEAR;
        sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ok = ok && vkCreateDescriptorPool(c.device, &dp, nullptr, &g->fsrPool) == VK_SUCCESS;
        ai.descriptorPool = g->fsrPool;
        g->fsrOk = ok && vkAllocateDescriptorSets(c.device, &ai, g->fsrSets) == VK_SUCCESS &&
                   vkCreateSampler(c.device, &sc, nullptr, &g->fsrSampler) == VK_SUCCESS;
        fprintf(stderr, "[fenetre] agrandissement AMD FSR 1 %s\n", g->fsrOk ? "actif" : "indisponible (étirement linéaire)");
    }
    if (!g->fsrOk) return false;
    if (g->fsr[0].width != outW || g->fsr[0].height != outH) {
        for (auto& im : g->fsr) {
            if (im.image) c.DestroyImage(im);
            if (!c.CreateImage(outW, outH, VK_FORMAT_R8G8B8A8_UNORM,
                               VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &im))
                return g->fsrOk = false;
        }
        g->ctx->Submit([&](VkCommandBuffer cmd) {
            for (auto& im : g->fsr) vk::Context::Transition(cmd, im.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        });
    }
    return true;
}

void WaitFrame() {
    if (!g->inFlight) return;
    vkWaitForFences(g->ctx->device, 1, &g->fence, VK_TRUE, UINT64_MAX);
    g->inFlight = false;
}

void DestroySwapchain() {
    for (VkSemaphore s : g->done) vkDestroySemaphore(g->ctx->device, s, nullptr);
    g->done.clear();
    g->images.clear();
    if (g->swapchain) vkDestroySwapchainKHR(g->ctx->device, g->swapchain, nullptr);
    g->swapchain = VK_NULL_HANDLE;
}

bool CreateSwapchain(uint32_t w, uint32_t h) {
    VkDevice dev = g->ctx->device;
    VkPhysicalDevice phys = g->ctx->physical;
    { std::lock_guard<std::mutex> l(g->ctx->queueLock); vkDeviceWaitIdle(dev); }   // l'ancienne chaîne n'est plus utilisée (présentations comprises)
    DestroySwapchain();
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, g->surface, &caps) != VK_SUCCESS) return false;
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = std::clamp(w, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(h, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) return false;   // fenêtre réduite
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        fprintf(stderr, "[fenetre] surface sans copie de transfert : présentation impossible\n");
        return false;
    }
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, g->surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, g->surface, &n, formats.data());
    // Formats UNORM : les octets du tampon d'image (déjà en gamma) sont recopiés tels quels
    VkSurfaceFormatKHR chosen{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    for (auto& f : formats) {
        if (f.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) continue;
        if (f.format != VK_FORMAT_B8G8R8A8_UNORM && f.format != VK_FORMAT_R8G8B8A8_UNORM) continue;
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(phys, f.format, &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) { chosen = f; break; }
    }
    if (chosen.format == VK_FORMAT_UNDEFINED) {
        fprintf(stderr, "[fenetre] aucun format de surface BGRA8/RGBA8 UNORM utilisable\n");
        return false;
    }
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, g->surface, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, g->surface, &n, modes.data());
    // Le jeu se cadence sur son propre vblank (60 Hz) : la présentation ne doit pas bloquer le GPU invité
    // (option vsync : FIFO, attente du balayage de l'écran)
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!config::g.vsync)
        for (VkPresentModeKHR want : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR})
            if (std::find(modes.begin(), modes.end(), want) != modes.end()) { mode = want; break; }

    fprintf(stderr, "[fenetre] mode de présentation %s\n", mode == VK_PRESENT_MODE_FIFO_KHR ? "FIFO (vsync)" :
            mode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : "IMMEDIATE");
    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = g->surface;
    sci.minImageCount = caps.minImageCount + 1;
    if (caps.maxImageCount && sci.minImageCount > caps.maxImageCount) sci.minImageCount = caps.maxImageCount;
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & sci.compositeAlpha))
        for (uint32_t bit = 1; bit <= VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR; bit <<= 1)
            if (caps.supportedCompositeAlpha & bit) { sci.compositeAlpha = VkCompositeAlphaFlagBitsKHR(bit); break; }
    sci.presentMode = mode;
    sci.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(dev, &sci, nullptr, &g->swapchain) != VK_SUCCESS) {
        fprintf(stderr, "[fenetre] vkCreateSwapchainKHR a échoué\n");
        return false;
    }
    vkGetSwapchainImagesKHR(dev, g->swapchain, &n, nullptr);
    g->images.resize(n);
    vkGetSwapchainImagesKHR(dev, g->swapchain, &n, g->images.data());
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    g->done.resize(n);
    for (auto& s : g->done) vkCreateSemaphore(dev, &si, nullptr, &s);
    g->format = chosen.format;
    g->extent = extent;
    fprintf(stderr, "[fenetre] chaîne d'échange %ux%u, %u images, mode de présentation %d\n", extent.width, extent.height, n, int(mode));
    g->stale = false;
    return true;
}

// Image source à la taille de l'image du jeu (après WaitFrame : plus utilisée)
bool EnsureSource(uint32_t w, uint32_t h) {
    vk::Context& c = *g->ctx;
    if (g->source.width != w || g->source.height != h) {
        if (g->source.image) c.DestroyImage(g->source);
        if (!c.CreateImage(w, h, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &g->source))   // échantillonnage : la vue créée par CreateImage l'exige
            return false;
    }
    return true;
}
}  // namespace

bool PresentInit(vk::Context* ctx, VkSurfaceKHR surface, std::string* error) {
    auto fail = [&](const char* m) { if (error) *error = m; return false; };
    if (!ctx->swapchain) return fail("VK_KHR_swapchain absente");
    PFN_vkGetInstanceProcAddr gipa = vk::InstanceProcAddr();
#define WK_LOAD_INSTANCE(name) name = reinterpret_cast<PFN_##name>(gipa(ctx->instance, #name));
    WK_PRESENT_INSTANCE(WK_LOAD_INSTANCE)
#undef WK_LOAD_INSTANCE
#define WK_LOAD_DEVICE(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(ctx->device, #name));
    WK_PRESENT_DEVICE(WK_LOAD_DEVICE)
#undef WK_LOAD_DEVICE
    if (!vkGetPhysicalDeviceSurfaceSupportKHR || !vkCreateSwapchainKHR) return fail("fonctions de présentation absentes");
    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(ctx->physical, ctx->queueFamily, surface, &supported);
    if (!supported) return fail("la file graphique ne peut pas présenter sur cette surface");
    g = new State();
    g->ctx = ctx;
    g->surface = surface;
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(ctx->device, &si, nullptr, &g->acquired);
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(ctx->device, &fi, nullptr, &g->fence);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = ctx->pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    vkAllocateCommandBuffers(ctx->device, &ai, &g->cmd);
    return true;
}

void PresentResize() { if (g) g->stale = true; }

void AnnoncerCapture() { g_annonceFin = MaintenantNs() + 2000000000LL; }

void PresentFrameImage(VkImage native, uint32_t w, uint32_t h, uint32_t drawableW, uint32_t drawableH) {
    if (!g || !w || !h) return;
    WaitFrame();
    if ((g->stale || !g->swapchain) && !CreateSwapchain(drawableW, drawableH)) return;
    if (!EnsureSource(w, h)) { fprintf(stderr, "[fenetre] allocation de l'image source impossible\n"); return; }

    VkDevice dev = g->ctx->device;
    uint32_t index = 0;
    VkResult r = vkAcquireNextImageKHR(dev, g->swapchain, UINT64_MAX, g->acquired, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { g->stale = true; return; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { fprintf(stderr, "[fenetre] acquisition : erreur %d\n", int(r)); g->stale = true; return; }
    if (r == VK_SUBOPTIMAL_KHR) g->stale = true;   // présentée quand même, recréée ensuite

    VkCommandBuffer cmd = g->cmd;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    VkImage dst = g->images[index];
    // image native : lots natifs soumis avant sur la même file, barrière globale
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    vk::Context::Transition(cmd, g->source.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit b{};   // blit : conversion de format éventuelle (cible native B8G8R8A8, 10 bits…)
    b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.srcOffsets[1] = b.dstOffsets[1] = {int32_t(w), int32_t(h), 1};
    vkCmdBlitImage(cmd, native, VK_IMAGE_LAYOUT_GENERAL, g->source.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
    // Post-traitement (options [post]) : passes de calcul ; l'étirement part ensuite de leur résultat
    VkImage blitSrc = g->source.image;
    VkImageLayout blitLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (PostWanted() && EnsurePost(w, h)) {
        vk::Context::Transition(cmd, g->source.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g->postPipe);
        const auto& o = config::g;
        for (uint32_t pass = o.antiAliasing == "fxaa" ? 0 : 1; pass < 2; pass++) {
            struct { uint32_t pass; float sharp, bright, gamma; uint32_t xbox; } pc{pass, o.sharpness, o.brightness, o.gamma, o.xboxColor ? 1u : 0u};
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g->postLayout, 0, 1, &g->postSets[pass], 0, nullptr);
            vkCmdPushConstants(cmd, g->postLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
            vkCmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
            vk::Context::Transition(cmd, g->post[pass].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);   // barrière
        }
        blitSrc = g->post[1].image;
        blitLayout = VK_IMAGE_LAYOUT_GENERAL;
    } else {
        vk::Context::Transition(cmd, g->source.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
    // Proportions de l'image du jeu conservées, centrée
    const uint32_t W = g->extent.width, H = g->extent.height;
    uint32_t dw = W, dh = uint32_t(uint64_t(W) * h / w);
    if (dh > H) { dh = H; dw = uint32_t(uint64_t(H) * w / h); }
    const int32_t x0 = int32_t((W - dw) / 2), y0 = int32_t((H - dh) / 2);
    // Option agrandissement = "fsr1" : EASU (w x h -> dw x dh) puis RCAS, copie 1:1 ensuite (sinon étirement linéaire)
    uint32_t srcW = w, srcH = h;
    if (config::g.upscale == "fsr1" && dw && dh && (dw != w || dh != h) && EnsureFsr(dw, dh)) {
        VkImageView srcView = blitSrc == g->source.image ? g->source.view : g->post[1].view;
        if (blitLayout != VK_IMAGE_LAYOUT_GENERAL)
            vk::Context::Transition(cmd, blitSrc, blitLayout, VK_IMAGE_LAYOUT_GENERAL);
        VkDescriptorImageInfo in[2] = {{g->fsrSampler, srcView, VK_IMAGE_LAYOUT_GENERAL},
                                       {g->fsrSampler, g->fsr[0].view, VK_IMAGE_LAYOUT_GENERAL}};
        VkDescriptorImageInfo out[2] = {{VK_NULL_HANDLE, g->fsr[0].view, VK_IMAGE_LAYOUT_GENERAL},
                                        {VK_NULL_HANDLE, g->fsr[1].view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet wr[4] = {};
        for (int i = 0; i < 4; i++) {   // mis à jour à chaque image (WaitFrame : jeux libres) : source recréée possible
            wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr[i].dstSet = g->fsrSets[i / 2];
            wr[i].dstBinding = uint32_t(i % 2);
            wr[i].descriptorCount = 1;
            wr[i].descriptorType = i % 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wr[i].pImageInfo = i % 2 ? &out[i / 2] : &in[i / 2];
        }
        vkUpdateDescriptorSets(g->ctx->device, 4, wr, 0, nullptr);
        uint32_t con[16];
        for (int pass = 0; pass < 2; pass++) {
            if (pass == 0) Fsr1EasuConstants(con, w, h, dw, dh);
            else Fsr1RcasConstants(con, 0.2f);   // netteté recommandée par AMD
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g->fsrPipes[pass]);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g->fsrLayout, 0, 1, &g->fsrSets[pass], 0, nullptr);
            vkCmdPushConstants(cmd, g->fsrLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof con, con);
            vkCmdDispatch(cmd, (dw + 15) / 16, (dh + 15) / 16, 1);
            vk::Context::Transition(cmd, g->fsr[pass].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);   // barrière
        }
        blitSrc = g->fsr[1].image;
        blitLayout = VK_IMAGE_LAYOUT_GENERAL;
        srcW = dw;
        srcH = dh;
    }
    vk::Context::Transition(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkClearColorValue black{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    vk::Context::Transition(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {int32_t(srcW), int32_t(srcH), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {x0, y0, 0};
    blit.dstOffsets[1] = {x0 + int32_t(dw), y0 + int32_t(dh), 1};
    vkCmdBlitImage(cmd, blitSrc, blitLayout, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    const uint32_t e = std::max<uint32_t>(1, g->extent.height / 360);   // 3 en 1080p
    const bool annonce = MaintenantNs() < g_annonceFin.load();
    const bool texte = annonce && EnsureTexte(e);
    static bool dit = false;   // échec signalé une fois par message
    if (annonce && !texte && !dit) fprintf(stderr, "[capture] message « Capture effectuée » impossible (tampon)\n");
    dit = annonce;
    if (texte && g->texteW + 8 * e <= g->extent.width && g->texteH + 8 * e <= g->extent.height) {
        vk::Context::Transition(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageOffset = {int32_t(8 * e), int32_t(8 * e), 0};
        c.imageExtent = {g->texteW, g->texteH, 1};
        vkCmdCopyBufferToImage(cmd, g->texteBuf, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    }
    vk::Context::Transition(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    vkEndCommandBuffer(cmd);

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &g->acquired;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &g->done[index];
    vkResetFences(dev, 1, &g->fence);
    std::unique_lock<std::mutex> fileLock(g->ctx->queueLock);
    if (vkQueueSubmit(g->ctx->queue, 1, &si, g->fence) != VK_SUCCESS) { fprintf(stderr, "[fenetre] soumission échouée\n"); return; }
    g->inFlight = true;
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g->done[index];
    pi.swapchainCount = 1;
    pi.pSwapchains = &g->swapchain;
    pi.pImageIndices = &index;
    r = vkQueuePresentKHR(g->ctx->queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) g->stale = true;
    else if (r != VK_SUCCESS) fprintf(stderr, "[fenetre] présentation : erreur %d\n", int(r));
}

}  // namespace wk::gpu
