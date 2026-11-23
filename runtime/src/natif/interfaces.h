// B3 : interfaces entre le cœur natif (device.cpp) et le raccord (natif/raccord.cpp) : shaders précompilés
// (XenosRecomp -> SPIR-V, docs/natif-b3-shaders.md), sommets et tampons (natif/sommets, natif/tampons), textures
// (natif/textures). Faux : le draw est ignoré et compté par cause (ligne [natif N]).
// Appelées sur le fil du rendu natif, sauf ShaderVu et ShaderCree (fil du jeu, crochets D3D).
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <vector>
#include "natif/etats.h"

namespace wk::natif {

// État D3D relevé dans le device invité au moment du draw (avant l'appel de l'original, qui vide les bits dirty).
struct Instantane {
    uint32_t type = 0;           // 0 DrawVertices, 1 DrawIndexedVertices, 2 DrawVerticesUP, 3 DrawIndexedVerticesUP
    uint32_t prim = 0;           // D3DPRIMITIVETYPE
    uint32_t debut = 0;          // startVertex / startIndex
    uint32_t nombre = 0;         // sommets (non indexés) ou indices
    int32_t baseSommet = 0;      // baseVertexIndex (indexés) / minVertexIndex (UP indexés)
    uint32_t sommetsUP = 0, pasUP = 0, formatIndicesUP = 0;   // UP : nombre de sommets, stride, format des indices
    std::vector<uint8_t> donneesUP;   // UP : sommets puis indices recopiés (la mémoire de l'appelant est temporaire)
    uint32_t vs = 0, ps = 0;     // objets shader invités (dev+0x3290 / 0x328C)
    uint32_t declaration = 0;    // dev+0x2F58
    uint32_t indices = 0;        // IB courant dev+0x318C (bit 31 de IB+0 : 32 bits)
    uint32_t flux[16]{}, pas[16]{};    // VB dev+0x31A4+4s, stride dev+0x31E8+s
    uint32_t textures[26]{};     // dev+0x31F8+4n
    uint32_t fetch[192]{};       // constantes de lecture dev+0x480 (32 × 6 mots : textures et flux de sommets)
    uint32_t booleens[8]{}, entiers[32]{};   // dev+0x2780 (VS, PS) / dev+0x27A0 (VS, PS)
    // Constantes float VS (0..1023) puis PS (1024..2047), petit-boutistes ; partagées entre draws tant que rien ne change.
    std::shared_ptr<const std::array<float, 2048>> constantes;
    RegsEtat regs;
    float viewport[6]{};         // X, Y, W, H, MinZ, MaxZ (dev+12896)
    uint32_t vte = 0x43F;        // PA_CL_VTE_CNTL dev+0x294C (sans échelle X/Y : sommets déjà en pixels, ex. vidéo XMV)
    int32_t scissor[4]{};        // dev+12924 (gauche, haut, droite, bas)
    bool scissorActif = false;   // dev+12160
    float constanteMelange[4]{}; // RB_BLEND_RGBA dev+0x28E0
    float refAlpha = 0;          // RB_ALPHA_REF dev+0x2904 (0..1)
};

// --- Shaders (agent b3-shaders) ---
struct ShadersNatifs {
    VkShaderModule vs = VK_NULL_HANDLE, ps = VK_NULL_HANDLE;
    VkPipelineLayout disposition = VK_NULL_HANDLE;
    const VkSpecializationInfo* specialisation = nullptr;   // constant_id 0 (test alpha), pour les deux étages
    uint64_t cle = 0;            // identifie la paire et la spécialisation (cache des pipelines)
    uint32_t echec = 0;          // si faux : 1 shader non vu à sa création, 2 SPIR-V absent, 3 GPU sans les fonctions
    // Lie constantes, textures et échantillonneurs pour ce draw (pipeline déjà lié).
    // 0 : tout lié ; 1 : lié, une texture au moins de secours ; 2 : rien de lié (draw à ignorer).
    int (*lier)(VkCommandBuffer, VkPipelineLayout, const Instantane&) = nullptr;
};
bool ObtenirShaders(const Instantane& d, ShadersNatifs* out);
void ShaderVu(uint32_t shader, bool pixel);   // SetVertexShader/SetPixelShader : traduction anticipée possible
// Après l'original de CreateVertexShader 821AE0A8 / CreatePixelShader 821ADEC0 : empreinte XXH3 du conteneur.
void ShaderCree(uint32_t objet, uint32_t conteneur);

// --- Sommets (agent b3-sommets, runtime/src/natif/) ---
struct SommetsNatifs {
    const VkPipelineVertexInputStateCreateInfo* entree = nullptr;   // durée de vie : celle de la déclaration
    uint64_t cle = 0;            // identifie l'entrée de sommets (cache des pipelines)
    uint32_t echec = 0;          // si faux : 1 déclaration absente ou vide, 2 tampon de sommets ou d'indices indisponible
    VkExtent2D cible{};          // entrée : taille de la cible (sommets en pixels quand PA_CL_VTE_CNTL est sans échelle X/Y)
    // Lie tampons de sommets et d'indices puis émet le vkCmdDraw* (pipeline et liaisons des shaders faits).
    void (*dessiner)(VkCommandBuffer, const Instantane&) = nullptr;
};
bool ObtenirSommets(const Instantane& d, SommetsNatifs* out);

// --- Fournis par le cœur (device.cpp) ---
// Vue de l'image native écrite par un resolve natif dans la texture dont `mot1` est le mot 1 de la constante de
// lecture (adresse de base), nulle sinon (le draw prend alors le cache de textures natif).
VkImageView VueResolue(uint32_t mot1);
VkCommandBuffer CmdEnvoi();   // commandes d'envoi du lot courant (hors passe), soumises avant celles des draws
// Fourni par le raccord : appelé quand le cœur commence un lot `slot` (0/1, sa barrière attendue) de l'image `image`.
void NouveauLot(uint32_t slot, uint32_t image);

}  // namespace wk::natif
