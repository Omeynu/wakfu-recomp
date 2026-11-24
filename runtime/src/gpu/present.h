// Présentation dans une fenêtre : chaîne d'échange Vulkan sur une surface fournie par la fenêtre.
// Un seul thread : le fil natif.
#pragma once
#include <cstdint>
#include <string>
#include "vk.h"

namespace wk::gpu {

// `ctx` doit avoir VK_KHR_swapchain (vk::SetInstanceExtensions avant son Init) ; `surface` lui appartient ensuite.
bool PresentInit(vk::Context* ctx, VkSurfaceKHR surface, std::string* error);
// Image native (layout GENERAL, écrite par des lots soumis avant sur la même file) recopiée puis mise à l'échelle en
// gardant ses proportions (bandes noires) dans la zone dessinable.
void PresentFrameImage(VkImage image, uint32_t w, uint32_t h, uint32_t drawableW, uint32_t drawableH);
void PresentResize();
// Capture écrite (touche_capture) : « Capture effectuée » affiché 2 s en haut à gauche de la fenêtre. Tout fil.
void AnnoncerCapture();   // taille de la fenêtre changée : chaîne d'échange recréée à la prochaine image

}  // namespace wk::gpu
