// Présentation dans une fenêtre : chaîne d'échange Vulkan sur une surface fournie par la fenêtre.
// Un seul thread (celui du processeur de commandes, comme le rendu) : la file Vulkan n'est pas partagée.
#pragma once
#include <cstdint>
#include <string>
#include "vk.h"

namespace wk::gpu {

// `ctx` doit avoir VK_KHR_swapchain (vk::SetInstanceExtensions avant son Init) ; `surface` lui appartient ensuite.
bool PresentInit(vk::Context* ctx, VkSurfaceKHR surface, std::string* error);
// Image RGBA (w x h) mise à l'échelle en gardant ses proportions (bandes noires) dans la zone dessinable.
void PresentFrame(const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t drawableW, uint32_t drawableH);
struct TextureInfo;
// Tampon d'image 8_8_8_8 décodé sur GPU depuis la mémoire invitée (sans relecture CPU) ; faux si indisponible
// (l'appelant décode alors sur CPU et appelle PresentFrame).
bool PresentFrameGpu(const TextureInfo& t, uint32_t drawableW, uint32_t drawableH);
void PresentResize();   // taille de la fenêtre changée : chaîne d'échange recréée à la prochaine image

}  // namespace wk::gpu
