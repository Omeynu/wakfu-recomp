// N1 : cache de textures propre au rendu natif (docs/natif-plan.md §3 « Textures »). Clé = (adresse de base, adresse
// des mips, format, dimension, tailles, niveaux, pas, tuilage, mode d'octets) -> VkImage avec sa chaîne de mips
// (queue empaquetée Xenos comprise, mise en page de xenia texture_util.cc, BSD-3). Invalidation par génération
// (Lock/Unlock crochetés) ; éviction à DestructResource. Swizzle de la constante par la vue.
#pragma once
#include <cstdint>
#include "gpu/vk.h"

namespace wk::natif {

// --- Mise en page invitée (pure, testée par wakfu_unit_natif_textures) ---
constexpr uint32_t kNiveauxMax = 14;
struct Niveau {
    uint32_t adresse = 0;    // adresse physique (0xA0000000 | ...) du niveau ou de la queue qui le contient
    uint32_t pas = 0;        // pas en éléments (texels ou blocs 4x4)
    uint32_t ox = 0, oy = 0; // décalage dans la queue empaquetée, en éléments
    uint32_t w = 0, h = 0;   // taille en texels
};
struct Disposition {
    uint32_t format = 0, endian = 0, bpe = 0, bloc = 1, niveaux = 0;
    bool tuile = false;
    VkFormat vk = VK_FORMAT_UNDEFINED;   // format de l'image PC
    uint32_t conversion = 0;             // 0 copie, 1 16 bits -> RGBA8, 2 profondeur 24 bits -> R32F
    Niveau n[kNiveauxMax];
};
// false : format, dimension (cubes, 3D) ou adresses non gérés.
bool Disposer(const uint32_t f[6], Disposition* d);
// Octets du niveau `l` dans l'image PC (rangées contiguës d'éléments, après conversion).
uint32_t TailleNiveau(const Disposition& d, uint32_t l);
// Niveau `l` détuilé, mode d'octets appliqué et converti (TailleNiveau octets en `dst`).
void Convertir(const Disposition& d, uint32_t l, uint8_t* dst);

// --- Fil du jeu (crochets) ---
void TextureEcrite(uint32_t tex);     // LockRect / UnlockRect (en-tête de texture, constante de lecture en +28)
void TextureDetruite(uint32_t res);   // DestructResource 821BB548 (avant l'original)

// --- Fil natif ---
// Vue de la texture de la constante `f` ; nulle si non gérée (l'appelant lie alors VueVide).
VkImageView VueTexture(const uint32_t f[6]);
VkImageView VueVide();   // 1x1 noire transparente (emplacement sans texture)
void TexturesLot(uint32_t image);   // début d'un lot (raccord NouveauLot) : libérations différées, journal

}  // namespace wk::natif
