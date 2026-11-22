#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace wk::gpu {

struct TextureInfo {
    uint32_t type = 0, address = 0, width = 0, height = 0, pitch = 0, format = 0, endian = 0, tiled = 0;
    uint32_t swizzle = 0, dimension = 0;
};
TextureInfo ParseTextureFetch(const uint32_t fetch[6]);
// Décode en RGBA8 (largeur*hauteur*4) ; false et `why` si format ou dimension non gérés.
bool DecodeTexture(const TextureInfo& t, std::vector<uint8_t>* rgba, std::string* why);
// Empreinte de la mémoire source (0 si format non géré) : cache des textures entre les images.
uint64_t TextureSourceHash(const TextureInfo& t);
// Octets de mémoire source lus pour la texture (0 si format non géré).
uint64_t TextureSourceSpan(const TextureInfo& t);
// Profondeur k_24_8 / k_24_8_FLOAT -> flottants (R32_SFLOAT) ; false si autre format.
bool DecodeDepthTexture(const TextureInfo& t, std::vector<float>* out);

// Décodage sur GPU (calcul) reproduisant DecodeTexture / DecodeDepthTexture octet pour octet. Source GLSL sans
// #version ; avec DEPTH défini (formats 22/23) l'image de sortie est rgba32f, sinon rgba8. Liaisons : 0 = mémoire
// source (tampon de mots), 1 = image de sortie (largeur x hauteur de la texture) ; constantes poussées TexDecodeParams.
extern const char* const kTexDecodeGlsl;
struct TexDecodeParams { uint32_t baseWord, pitch, w, h, endian, tiled, swz, format; };
// Constantes pour une texture dont le premier octet est le mot `baseWord` du tampon lié, et groupes 8x8 à lancer ;
// false si le format ou la dimension ne sont pas gérés.
bool TexDecodeSetup(const TextureInfo& t, uint32_t baseWord, TexDecodeParams* p, uint32_t* groupsX, uint32_t* groupsY);

}  // namespace wk::gpu
