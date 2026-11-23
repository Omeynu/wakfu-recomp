// Rendu natif (B3) : tampons de sommets / d'indices invités -> tampons Vulkan (cf. docs/natif-b3-sommets.md).
// Statiques : DEVICE_LOCAL, recopiés via l'anneau d'envoi. Dynamiques (Unlock à moins de 4 images d'intervalle) :
// servis directement depuis l'anneau de l'image.
#pragma once
#include "gpu/vk.h"
#include "natif/sommets.h"

namespace natif {

struct Buffer { VkBuffer buffer = VK_NULL_HANDLE; VkDeviceSize offset = 0, size = 0; };   // size 0 : échec

bool BuffersInit(wk::vk::Context* vk, uint32_t frames = 2, VkDeviceSize ringBytesPerFrame = 64ull << 20);
void BuffersShutdown();   // après vkDeviceWaitIdle
// Début de l'image `frame % frames` : le cœur a attendu la barrière de cette image (anneau réutilisable).
void BuffersBeginFrame(uint64_t frame);
// Après l'original de UnlockVertexBuffer 821BAC30 / UnlockIndexBuffer 821BACF0 (pRes = r3 à l'entrée).
void OnUnlock(uint32_t guestRes);
// Avant l'original de DestructResource 821BB548 ; destruction Vulkan différée de `frames` images.
void OnDestroy(uint32_t guestRes);
// Tampon PC du flux `stream` de la déclaration `l`, données à partir de `offset` (SetStreamSource) : lier avec
// buffer/offset rendus (le décalage invité est déjà appliqué), pas PcStride(l, stream, stride).
// `upload` : commande hors vkCmdBeginRendering, soumise avant les draws (copies + barrières).
// `pixels` (largeur, hauteur de la cible) : positions en pixels converties par PixelsVersNdc.
Buffer BufferFor(VkCommandBuffer upload, uint32_t guestVb, const VertexLayout& l, uint32_t stream, uint32_t offset,
                 uint32_t stride, const float* pixels = nullptr);
Buffer IndexBufferFor(VkCommandBuffer upload, uint32_t guestIb, VkIndexType* type);
// Place de `size` octets dans l'anneau de l'image (sommets/indices UP) ; *dst nul et size 0 si plein.
Buffer RingUpload(VkDeviceSize size, uint8_t** dst);

}  // namespace natif
