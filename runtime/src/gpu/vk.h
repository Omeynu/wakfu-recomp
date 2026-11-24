// Vulkan chargé dynamiquement (libvulkan.so.1 / vulkan-1.dll) : pas de bibliothèque d'import,
// fonctionne en compilation croisée MinGW et sous Wine. Contexte sans écran (rendu hors écran).
#pragma once
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace wk::vk {

#define WK_VK_GLOBAL(X) X(vkCreateInstance) X(vkEnumerateInstanceLayerProperties)
#define WK_VK_INSTANCE(X)                                                                          \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceFeatures) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties)             \
    X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkGetPhysicalDeviceFormatProperties)
#define WK_VK_DEVICE(X)                                                                            \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateCommandPool) X(vkDestroyCommandPool)          \
    X(vkAllocateCommandBuffers) X(vkFreeCommandBuffers) X(vkBeginCommandBuffer)                    \
    X(vkEndCommandBuffer) X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkDeviceWaitIdle)                  \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory)         \
    X(vkFreeMemory) X(vkBindImageMemory) X(vkCreateBuffer) X(vkDestroyBuffer)                     \
    X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory)          \
    X(vkCmdPipelineBarrier) X(vkCmdClearColorImage) X(vkCmdCopyImageToBuffer)                    \
    X(vkCmdCopyBufferToImage) X(vkCmdCopyBuffer) X(vkCreateImageView) X(vkDestroyImageView)                          \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreatePipelineLayout) X(vkCreatePipelineCache) X(vkGetPipelineCacheData)                   \
    X(vkDestroyPipelineLayout) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline)                   \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreateDescriptorPool)        \
    X(vkDestroyDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets)               \
    X(vkCreateQueryPool) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults)     \
    X(vkCreateSampler) X(vkDestroySampler) X(vkCmdBeginRendering) X(vkCmdEndRendering)             \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers)                      \
    X(vkCmdBindIndexBuffer) X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdSetViewport) X(vkCmdSetScissor) \
    X(vkCmdPushConstants) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkGetFenceStatus)          \
    X(vkResetDescriptorPool) X(vkCmdClearAttachments)   \
    X(vkCreateComputePipelines) X(vkCmdDispatch) X(vkCmdClearDepthStencilImage) X(vkCmdSetStencilReference) \
    X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdBlitImage) X(vkResetCommandBuffer) X(vkCreateSemaphore) X(vkDestroySemaphore) \
    X(vkCmdResolveImage) X(vkCmdCopyImage) X(vkCmdSetBlendConstants) X(vkGetBufferDeviceAddress)

#define WK_VK_DECLARE(name) extern PFN_##name name;
WK_VK_GLOBAL(WK_VK_DECLARE)
WK_VK_INSTANCE(WK_VK_DECLARE)
WK_VK_DEVICE(WK_VK_DECLARE)
#undef WK_VK_DECLARE

// Extensions d'instance supplémentaires (surface de la fenêtre), à fixer avant Context::Init.
// Non vide : VK_KHR_swapchain est aussi activée sur le périphérique si disponible (Context::swapchain).
void SetInstanceExtensions(std::vector<std::string> names);
PFN_vkGetInstanceProcAddr InstanceProcAddr();   // après Context::Init

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
};

// Aspect d'une vue selon le format (profondeur + stencil ou couleur).
inline VkImageAspectFlags ImageAspect(VkFormat f) {
    return f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT
               ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT);
}
struct Context {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    std::mutex queueLock;               // soumissions : fil natif et présentation
    uint32_t queueFamily = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    std::string deviceName;
    bool validation = false;
    bool bc = false;                    // textureCompressionBC activée (textures DXT natives)
    float maxAnisotropy = 0;            // samplerAnisotropy activée : plafond du GPU (0 : non prise en charge)
    int deviceType = 0;                 // VkPhysicalDeviceType (config::MsaaEchantillons)
    VkSampleCountFlags sampleCounts = VK_SAMPLE_COUNT_1_BIT;   // MSAA possible sur les cibles couleur et profondeur
    float timestampPeriod = 0;
    uint64_t timestampMask = ~0ull;     // bits valides des horodatages de la file graphique          // ns par tic d'horodatage (0 : horodatages non pris en charge)
    bool swapchain = false;             // VK_KHR_swapchain activée (présentation dans une fenêtre)
    bool natifShaders = false;          // bufferDeviceAddress + runtimeDescriptorArray + shaderInt64 (shaders natifs)
    std::string natifManque;            // fonctions absentes parmi celles-ci (message au joueur)

    // Chargement de Vulkan, instance (validation si WAKFU_VK_VALIDATION=1), premier GPU capable.
    bool Init(std::string* error);
    void Shutdown();

    uint32_t FindMemory(uint32_t typeBits, VkMemoryPropertyFlags props) const;
    bool CreateImage(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage, Image* out, VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);
    void DestroyImage(Image& img);
    // Commandes ponctuelles : enregistre via `record`, soumet, attend.
    template <class F> void Submit(F record);
    // Recopie une image (couleur, 4 octets/texel, mise en page TRANSFER_SRC) en RGBA.
    bool Readback(const Image& img, VkImageLayout layout, std::vector<uint8_t>* rgba);
    // Transition de mise en page (barrière complète, simple).
    static void Transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                           VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);

    VkCommandBuffer BeginOneShot();
    void EndOneShot(VkCommandBuffer cmd);
};

template <class F> void Context::Submit(F record) {
    VkCommandBuffer cmd = BeginOneShot();
    record(cmd);
    EndOneShot(cmd);
}

}  // namespace wk::vk
