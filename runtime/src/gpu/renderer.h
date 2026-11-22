#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace wk::vk { struct Context; }
namespace wk::gpu {

struct DrawPacket {
    uint32_t initiator = 0;     // VGT_DRAW_INITIATOR
    uint32_t indexBase = 0;     // adresse physique des indices (DRAW_INDX avec DMA)
    uint32_t indexInfo = 0;     // mot de taille/mode d'octets des indices
    bool dma = false;
};
struct RendererFrameStats {
    uint32_t drawn = 0, skipped = 0, resolves = 0, resolvesSkipped = 0, clears = 0, textureUploads = 0, textureFailures = 0, batches = 0, waits = 0;   // lots soumis, attentes bloquantes
    uint32_t waitsOverlap = 0, waitsSync = 0, waitsRing = 0;
    uint32_t gpuCatUs[6] = {};   // WAKFU_GPU_PROFIL : draws, décodages, envois, resolves, effacements, reste (µs)
    uint32_t gpuUs = 0;   // temps GPU des lots retirés pendant l'image (horodatages), en µs   // dont : texture CPU après resolve, attente du CP, 3 lots en vol
};

bool RendererInit(std::string* error);   // Vulkan + mémoire invitée importée
bool RendererActive();
vk::Context* RendererContext();          // contexte Vulkan du rendu (nul si inactif)
void RendererDraw(const uint32_t* regs, uint64_t vsHash, uint64_t psHash, const DrawPacket& d);
void RendererResolve(const uint32_t* regs);
// Termine tout le travail GPU enregistré (soumis ou non) et applique les écritures différées.
void RendererSyncCpu();
// Écriture visible du CPU (barrière, compteur, interruption) : si du travail GPU est enregistré ou en vol, `op` est
// rangé avec le dernier lot (soumis aussitôt, sans attente) et exécuté à sa fin réelle, dans l'ordre ; retourne
// faux si le GPU est au repos (à exécuter tout de suite).
bool RendererDefer(std::function<void()> op);
// Attend (bloquant, au plus `timeoutNs`) la fin du plus ancien lot en vol et retire les lots finis ;
// faux si aucun lot n'était en vol.
bool RendererWait(uint64_t timeoutNs);
void RendererSavePipelineCache();          // cache de pipelines sur disque (sorties propres : quit.h)
RendererFrameStats RendererEndFrame();     // soumet sans attendre, remet les compteurs à zéro

}  // namespace wk::gpu
