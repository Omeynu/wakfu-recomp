// Test du socle Vulkan sans écran : effacement d'une image et relecture.
#include <cstdio>
#include "../src/gpu/vk.h"
using namespace wk::vk;
int main() {
    Context c;
    std::string err;
    if (!c.Init(&err)) { printf("ÉCHEC init : %s\n", err.c_str()); return 1; }
    printf("GPU : %s (validation %s)\n", c.deviceName.c_str(), c.validation ? "oui" : "non");
    Image img;
    if (!c.CreateImage(64, 32, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &img)) { printf("ÉCHEC image\n"); return 1; }
    c.Submit([&](VkCommandBuffer cmd) {
        Context::Transition(cmd, img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue col{};
        col.float32[0] = 1.0f; col.float32[1] = 0.5f; col.float32[2] = 0.0f; col.float32[3] = 1.0f;
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &col, 1, &r);
        Context::Transition(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    });
    std::vector<uint8_t> px;
    if (!c.Readback(img, VK_IMAGE_LAYOUT_GENERAL, &px)) { printf("ÉCHEC relecture\n"); return 1; }
    const bool ok = px.size() == 64 * 32 * 4 && px[0] == 255 && (px[1] == 127 || px[1] == 128) && px[2] == 0 && px[3] == 255 &&
                    px[px.size() - 4] == 255;
    c.DestroyImage(img);
    c.Shutdown();
    printf(ok ? "OK\n" : "ÉCHEC pixels %u %u %u %u\n", px[0], px[1], px[2], px[3]);
    return ok ? 0 : 1;
}
