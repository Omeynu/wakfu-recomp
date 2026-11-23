// Rendu natif : les fonctions D3D du jeu, crochetées dans d3d_ombre.cpp, sont rendues par Vulkan dans des images
// natives, sur un fil à part (file de commandes, comme Video:: d'UnleashedRecomp) ; l'image passée à Swap est présentée
// dans la fenêtre. Le CP (kernel/gpu_cp.cpp) suit le flux PM4 sans rendre (barrières, scratch, interruptions, VdSwap) ;
// ses barrières passent par Rappel. Capture : WAKFU_CAPTURE (numéros de VdSwap). Détail : docs/natif.md.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace wk::vk { struct Context; }
namespace wk::natif {
struct Instantane;

// Fil du CP, après window::Open (extensions de surface) : contexte Vulkan du rendu ; sans lui, rien n'est rendu.
bool InitialiserVulkan(std::string* erreur);
// Rendu impossible (GPU, Vulkan, shaders précompilés) : message au joueur (boîte sous Windows, journal), puis arrêt.
[[noreturn]] void RenduImpossible(const std::string& cause);
// Fil du jeu : la texture est la destination d'un resolve natif (son contenu n'est pas recopié en mémoire invitée).
bool DestinationResolve(uint32_t tex);
// Arrêt (WAKFU_STOP_AT_SWAP) : attend, 10 s au plus, que le fil natif ait traité les images déjà envoyées (captures).
void Vider();
// Fil du CP : `op` (écriture visible du CPU) exécutée sur le fil natif quand tout le travail natif envoyé
// avant elle est fini sur le GPU (le jeu réécrit sommets et constantes après ses barrières).
void Rappel(std::function<void()> op);
vk::Context* Contexte();   // fil natif : contexte Vulkan (nul avant l'initialisation du fil)

// Fil du jeu, appelées par les crochets (arguments = registres de l'appel D3D)
void CreationSurface(uint32_t surf, uint32_t w, uint32_t h, uint32_t format, uint32_t multiSample);
void CreationTexture(uint32_t tex, uint32_t usage);
void CibleCouleur(uint32_t index, uint32_t surf);
void CibleProfondeur(uint32_t surf);
// Recopie les constantes VS/PS marquées dirty (le jeu les écrit aussi en ligne dans le device) puis vide ces bits : à
// chaque draw (Relever) et avant le vidage autonome 821B9390.
void SynchroConstantes(uint32_t dev);
void Effacer(uint32_t flags, uint32_t rect, uint32_t couleur, float z, uint32_t stencil);
void Resoudre(uint32_t flags, uint32_t rect, uint32_t tex, uint32_t point, uint32_t niveau, uint32_t couleur, float z,
              uint32_t stencil);
// État du device relevé dans `d` (avant l'original : il vide les bits dirty).
void Relever(uint32_t dev, Instantane* d);
// `d` : champs du draw remplis par le crocheteur ; le reste est lu dans le device. UP : données invitées à recopier.
// dev nul : état déjà relevé par Relever (BeginVertices XUI, sommets écrits avant EndVertices).
void Dessiner(uint32_t dev, std::unique_ptr<Instantane> d, uint32_t sommetsUP, uint32_t indicesUP);
void Echange(uint32_t frontTex);
// Le CP a atteint le swap `n` (calé sur le vblank) : l'image native n peut être présentée.
void SwapCp(uint32_t n);

}  // namespace wk::natif
