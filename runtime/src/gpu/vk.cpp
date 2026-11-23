#include "../config.h"
#include "vk.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace wk::vk {

#define WK_VK_DEFINE(name) PFN_##name name = nullptr;
WK_VK_GLOBAL(WK_VK_DEFINE)
WK_VK_INSTANCE(WK_VK_DEFINE)
WK_VK_DEVICE(WK_VK_DEFINE)
#undef WK_VK_DEFINE

namespace {
PFN_vkGetInstanceProcAddr g_getInstanceProcAddr = nullptr;
std::vector<std::string> g_instanceExtensions;

bool LoadLoader() {
    if (g_getInstanceProcAddr) return true;
#ifdef _WIN32
    HMODULE m = LoadLibraryA("vulkan-1.dll");
    if (m) g_getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void*>(GetProcAddress(m, "vkGetInstanceProcAddr")));
#else
    void* m = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (m) g_getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(m, "vkGetInstanceProcAddr"));
#endif
    return g_getInstanceProcAddr != nullptr;
}
}  // namespace

void SetInstanceExtensions(std::vector<std::string> names) { g_instanceExtensions = std::move(names); }
PFN_vkGetInstanceProcAddr InstanceProcAddr() { return g_getInstanceProcAddr; }

bool Context::Init(std::string* error) {
    auto fail = [&](const char* m) { if (error) *error = m; return false; };
    if (!LoadLoader()) return fail("chargeur Vulkan introuvable");
#define WK_VK_LOAD_GLOBAL(name) name = reinterpret_cast<PFN_##name>(g_getInstanceProcAddr(nullptr, #name));
    WK_VK_GLOBAL(WK_VK_LOAD_GLOBAL)
#undef WK_VK_LOAD_GLOBAL
    if (!vkCreateInstance) return fail("vkCreateInstance absent");

    const char* layer = "VK_LAYER_KHRONOS_validation";
    validation = getenv("WAKFU_VK_VALIDATION") && *getenv("WAKFU_VK_VALIDATION") == '1';
    if (validation) {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> layers(n);
        vkEnumerateInstanceLayerProperties(&n, layers.data());
        bool found = false;
        for (auto& l : layers) found |= strcmp(l.layerName, layer) == 0;
        if (!found) { fprintf(stderr, "[vulkan] couche de validation absente\n"); validation = false; }
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "wakfu-recomp";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    if (validation) { ici.enabledLayerCount = 1; ici.ppEnabledLayerNames = &layer; }
    std::vector<const char*> instanceExts;
    for (auto& e : g_instanceExtensions) instanceExts.push_back(e.c_str());
    ici.enabledExtensionCount = uint32_t(instanceExts.size());
    ici.ppEnabledExtensionNames = instanceExts.data();
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) return fail("vkCreateInstance a échoué (Vulkan 1.3 requis)");
#define WK_VK_LOAD_INSTANCE(name) name = reinterpret_cast<PFN_##name>(g_getInstanceProcAddr(instance, #name));
    WK_VK_INSTANCE(WK_VK_LOAD_INSTANCE)
#undef WK_VK_LOAD_INSTANCE

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    // Préférence : GPU dédié, puis intégré, puis logiciel (lavapipe)
    // Option [video] gpu, sinon dédié > intégré > autre (règle partagée avec le lanceur, config::ChoisirGpu)
    std::vector<config::GpuVu> vus;
    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devices[i], &p);
        vus.push_back({p.deviceName, int(p.deviceType), p.apiVersion >= VK_API_VERSION_1_3});
    }
    bool trouve = true;
    const int best = config::ChoisirGpu(vus, config::g.gpu, &trouve);
    if (!trouve) fprintf(stderr, "[config] GPU « %s » introuvable : choix automatique\n", config::g.gpu.c_str());
    if (best < 0) return fail("aucun GPU Vulkan 1.3");
    physical = devices[best];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical, &props);
    deviceName = props.deviceName;
    deviceType = int(props.deviceType);
    timestampPeriod = props.limits.timestampComputeAndGraphics ? props.limits.timestampPeriod : 0.0f;

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &qn, qf.data());
    bool found = false;
    for (uint32_t i = 0; i < qn && !found; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            queueFamily = i;
            found = true;
            const uint32_t bits = qf[i].timestampValidBits;
            if (!bits) timestampPeriod = 0;
            else if (bits < 64) timestampMask = (1ull << bits) - 1;
        }
    if (!found) return fail("aucune file graphique");

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    // Extensions : chaîne d'échange (fenêtre)
    PFN_vkEnumerateDeviceExtensionProperties enumExt = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        g_getInstanceProcAddr(instance, "vkEnumerateDeviceExtensionProperties"));
    uint32_t en = 0;
    enumExt(physical, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> exts(en);
    enumExt(physical, nullptr, &en, exts.data());
    std::vector<const char*> enabled;
    if (!g_instanceExtensions.empty())
        for (auto& e : exts)
            if (!strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
                enabled.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
                swapchain = true;
            }
    // Accès hors limites des tampons : contenus (adresses invitées parfois farfelues)
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.features.robustBufferAccess = VK_TRUE;
    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(physical, &supported);
    f2.features.textureCompressionBC = supported.textureCompressionBC;
    bc = supported.textureCompressionBC;
    // Shaders natifs (B3, XenosRecomp) : constantes par adresse de tampon, tableaux de descripteurs sans borne
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    {
        VkPhysicalDeviceVulkan12Features s12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        q.pNext = &s12;
        auto getFeat2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(g_getInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2"));
        if (getFeat2) getFeat2(physical, &q);
        natifShaders = s12.bufferDeviceAddress && s12.runtimeDescriptorArray && supported.shaderInt64;
        for (auto [ok, nom] : {std::pair{s12.bufferDeviceAddress, "bufferDeviceAddress"}, {s12.runtimeDescriptorArray, "runtimeDescriptorArray"},
                               {supported.shaderInt64, "shaderInt64"}})
            if (!ok) natifManque += std::string(natifManque.empty() ? "" : ", ") + nom;
        f12.bufferDeviceAddress = f12.runtimeDescriptorArray = natifShaders;
        f2.features.shaderInt64 = natifShaders;
    }
    f12.pNext = &f13;
    f2.pNext = &f12;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.enabledExtensionCount = uint32_t(enabled.size());
    dci.ppEnabledExtensionNames = enabled.data();
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    if (vkCreateDevice(physical, &dci, nullptr, &device) != VK_SUCCESS) return fail("vkCreateDevice a échoué");
#define WK_VK_LOAD_DEVICE(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name));
    WK_VK_DEVICE(WK_VK_LOAD_DEVICE)
#undef WK_VK_LOAD_DEVICE
    vkGetDeviceQueue(device, queueFamily, 0, &queue);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queueFamily;
    if (vkCreateCommandPool(device, &pci, nullptr, &pool) != VK_SUCCESS) return fail("vkCreateCommandPool a échoué");
    return true;
}

void Context::Shutdown() {
    if (device) { vkDeviceWaitIdle(device); vkDestroyCommandPool(device, pool, nullptr); vkDestroyDevice(device, nullptr); }
    if (instance) vkDestroyInstance(instance, nullptr);
    device = VK_NULL_HANDLE;
    instance = VK_NULL_HANDLE;
}

uint32_t Context::FindMemory(uint32_t typeBits, VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(physical, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((typeBits >> i & 1) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    return UINT32_MAX;
}

bool Context::CreateImage(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage, Image* out, VkSampleCountFlagBits samples) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    if (vkCreateImage(device, &ci, nullptr, &out->image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, out->image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = FindMemory(req.memoryTypeBits, 0);
    if (vkAllocateMemory(device, &ai, nullptr, &out->memory) != VK_SUCCESS) return false;
    vkBindImageMemory(device, out->image, out->memory, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = out->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {ImageAspect(format), 0, 1, 0, 1};
    if (vkCreateImageView(device, &vi, nullptr, &out->view) != VK_SUCCESS) return false;
    out->format = format;
    out->width = w;
    out->height = h;
    return true;
}

void Context::DestroyImage(Image& img) {
    if (img.view) vkDestroyImageView(device, img.view, nullptr);
    if (img.image) vkDestroyImage(device, img.image, nullptr);
    if (img.memory) vkFreeMemory(device, img.memory, nullptr);
    img = Image{};
}

VkCommandBuffer Context::BeginOneShot() {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &ai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

void Context::EndOneShot(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    {
        std::lock_guard<std::mutex> l(queueLock);
        vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);
    }
    vkFreeCommandBuffers(device, pool, 1, &cmd);
}

void Context::Transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkImageAspectFlags aspect) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

bool Context::Readback(const Image& img, VkImageLayout layout, std::vector<uint8_t>* rgba) {
    const VkDeviceSize size = VkDeviceSize(img.width) * img.height * 4;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf;
    if (vkCreateBuffer(device, &bci, nullptr, &buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, buf, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory mem;
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device, &ai, nullptr, &mem) != VK_SUCCESS) {
        vkDestroyBuffer(device, buf, nullptr);
        return false;
    }
    vkBindBufferMemory(device, buf, mem, 0);
    Submit([&](VkCommandBuffer cmd) {
        Transition(cmd, img.image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {img.width, img.height, 1};
        vkCmdCopyImageToBuffer(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
        Transition(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, layout);
    });
    void* p;
    vkMapMemory(device, mem, 0, size, 0, &p);
    rgba->assign(static_cast<uint8_t*>(p), static_cast<uint8_t*>(p) + size);
    vkUnmapMemory(device, mem);
    vkFreeMemory(device, mem, nullptr);
    vkDestroyBuffer(device, buf, nullptr);
    return true;
}

}  // namespace wk::vk
