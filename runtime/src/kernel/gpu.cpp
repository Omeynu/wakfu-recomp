// Imports Vd* : anneau de commandes lu par le processeur de commandes (gpu_cp.cpp, sans rendu : le rendu est natif),
// rappel d'interruption appelé à 60 Hz (vblank), VdSwap marqué dans le flux.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include "../platform.h"
#include "../trace.h"
#include "../process.h"
#include "stub.h"
#include "sync.h"
#include "gpu_cp.h"
#include "window.h"
#ifdef WAKFU_HAS_SHADERGEN
#include "../natif/device.h"
#endif
#include <string>

using namespace wk;

namespace {
constexpr uint32_t kMmio = 0x7FC80000;               // registres GPU (MMIO), vus comme mémoire
constexpr uint32_t kRegWptr = kMmio + 0x01C5 * 4;     // CP_RB_WPTR
std::atomic<uint32_t> g_ringBase{0}, g_ringSizeLog2{0}, g_rptrWriteback{0};
std::atomic<uint32_t> g_irqCallback{0}, g_irqUser{0}, g_swaps{0};
const int g_stopAtSwap = getenv("WAKFU_STOP_AT_SWAP") ? atoi(getenv("WAKFU_STOP_AT_SWAP")) : 0;

// Adresse physique (vue GPU) -> adresse invitée : la mémoire physique est allouée en 0xA0000000+.
uint32_t PhysToGuest(uint32_t p) { return p < 0x20000000 ? 0xA0000000u | p : p; }
}  // namespace

static void OnGpuSwap(uint32_t n);

namespace wk {
void StartDpcThread();
// Journal du fil « processeur de commandes » (CP seul) : sous Windows une pause courte peut durer 1 à 15,6 ms.
double g_cpPausesMs = 0, g_cpAnneauMs = 0;
uint32_t g_cpPauses = 0;
extern double g_cpAttenteMemMs;   // kernel/gpu_cp.cpp
extern uint32_t g_cpAttentesMem;

void StartSystemThreads() {
    StartDpcThread();
    // Thread « processeur de commandes » : exécute l'anneau (barrières, interruptions CP, source 1). Il porte aussi
    // la fenêtre (création, événements) et crée le contexte Vulkan du rendu natif avant de rendre la main.
    GuestThread* cp = CreateGuestThread(0x10000);
    // Signal « prêt » : atomique + attente active courte (std::promise utilise call_once, introuvable sous MinGW + Clang)
    std::atomic<bool> cpReady{false};
    std::thread([cp, &cpReady] {
        PPCContext ctx{};
        AttachThread(cp, ctx);
        if (window::Wanted()) window::Open();
#ifdef WAKFU_HAS_SHADERGEN
        // Rendu : WAKFU_RENDER=1, ou par défaut avec une fenêtre
        const char* render = getenv("WAKFU_RENDER");
        if (render ? *render == '1' : window::IsOpen()) {
            std::string err;
            if (!wk::natif::InitialiserVulkan(&err)) wk::natif::RenduImpossible("initialisation Vulkan : " + err);
        }
#endif
        cpReady.store(true);
        uint32_t rptr = 0;
        CpSetCallbacks({[&ctx, cp](uint32_t source, uint32_t cpu) {
            if (uint32_t cb = g_irqCallback) {
                St8(cp->pcr + 0x10C, uint8_t(cpu));   // le gestionnaire efface le bit de « son » CPU
                ctx.r3.u64 = source;
                ctx.r4.u64 = g_irqUser;
                CallGuest(cb, ctx);
            }
        }, OnGpuSwap});
        for (;;) {
            if (g_ringBase) {
                const uint32_t size = 1u << (g_ringSizeLog2 + 1);   // en mots (2^(log2+3) octets)
                const uint32_t wptr = Ld32(kRegWptr) % size;
                if (wptr != rptr) {
                    trace::Mark('K', wptr);
                    const auto t0 = std::chrono::steady_clock::now();
                    rptr = CpProcessRing(PhysToGuest(g_ringBase), size, rptr, wptr);
                    g_cpAnneauMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                    trace::Mark('Q', rptr);
                }
                if (g_rptrWriteback) St32(PhysToGuest(g_rptrWriteback), rptr);
            }
            window::Pump();
            // Anneau vide : le jeu attend souvent une barrière que le CP doit lire au plus vite (BlockOnFence,
            // ≈ 1 image d'attente par image). 500 µs -> 20 µs : 17,6 -> 20,4-20,9 img/s (sauvegarde du joueur, zone 1).
            const auto t0 = std::chrono::steady_clock::now();
            platform::SleepUntil(std::chrono::steady_clock::now() + std::chrono::microseconds(20));   // Windows : minuteur haute résolution
            g_cpPausesMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            g_cpPauses++;
        }
    }).detach();
    while (!cpReady.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    // Thread « vblank » : 60 Hz, indépendant du processeur de commandes (source 0).
    GuestThread* vb = CreateGuestThread(0x10000);
    std::thread([vb] {
        PPCContext ctx{};
        AttachThread(vb, ctx);
        using Clock = std::chrono::steady_clock;
        auto next = Clock::now();
        auto bilan = next + std::chrono::seconds(10);
        double retardSomme = 0, retardMax = 0;   // retard des réveils sur l'échéance, en ms
        unsigned reveils = 0, enRetard = 0;
        uint32_t vblank = 0;
        for (;;) {
            next += std::chrono::microseconds(16667);
            platform::SleepUntil(next);
            double retard = std::chrono::duration<double, std::milli>(Clock::now() - next).count();
            retardSomme += retard; retardMax = std::max(retardMax, retard); ++reveils; enRetard += retard > 2.0;
            if (next >= bilan) {   // > 2 ms de retard : le jeu risque de rater la synchro (image de 33,3 ms)
                fprintf(stderr, "[vblank] 10 s : %u réveils, retard moyen %.2f ms, max %.2f ms, %u à plus de 2 ms\n",
                        reveils, retardSomme / reveils, retardMax, enRetard);
                bilan += std::chrono::seconds(10);
                retardSomme = retardMax = 0; reveils = enRetard = 0;
            }
            St32(kMmio + 0x6110, ++vblank);       // compteur de vblank lu par D3D
            St32(kMmio + 0x6544, 1);              // statut : bit 0 = vblank en cours (testé par le rappel)
            if (uint32_t cb = g_irqCallback) { ctx.r3.u64 = 0; ctx.r4.u64 = g_irqUser; CallGuest(cb, ctx); }
        }
    }).detach();
}
}  // namespace wk

WK_IMPORT(VdInitializeEngines) { WK_TRACE(VdInitializeEngines); Ret(ctx, 1); }
WK_IMPORT(VdShutdownEngines) { WK_TRACE(VdShutdownEngines); }
WK_IMPORT(VdSetGraphicsInterruptCallback) {
    WK_TRACE(VdSetGraphicsInterruptCallback);
    g_irqUser = ctx.r4.u32;
    g_irqCallback = ctx.r3.u32;
}
WK_IMPORT(VdSetSystemCommandBufferGpuIdentifierAddress) { WK_TRACE(VdSetSystemCommandBufferGpuIdentifierAddress); }
WK_IMPORT(VdInitializeRingBuffer) {
    WK_TRACE(VdInitializeRingBuffer);
    g_ringSizeLog2 = ctx.r4.u32;
    g_ringBase = ctx.r3.u32;
    fprintf(stderr, "[gpu] anneau %08X, taille 2^%u\n", ctx.r3.u32, ctx.r4.u32);
}
WK_IMPORT(VdEnableRingBufferRPtrWriteBack) {
    WK_TRACE(VdEnableRingBufferRPtrWriteBack);
    g_rptrWriteback = ctx.r3.u32;
    fprintf(stderr, "[gpu] recopie du pointeur de lecture vers %08X\n", ctx.r3.u32);
}
WK_IMPORT(VdGetSystemCommandBuffer) {
    WK_TRACE(VdGetSystemCommandBuffer);
    static uint32_t buf = Alloc(Heap::Physical, 0x10000, 0x1000);
    St32(ctx.r3.u32, buf);
    St32(ctx.r4.u32, 0xBEEF0001);
}
static void FillVideoMode(uint32_t m);
WK_IMPORT(VdQueryVideoMode) { WK_TRACE(VdQueryVideoMode); FillVideoMode(ctx.r3.u32); }
WK_IMPORT(XGetVideoMode) { WK_TRACE(XGetVideoMode); FillVideoMode(ctx.r3.u32); }
static void FillVideoMode(uint32_t m) {
    for (uint32_t i = 0; i < 0x30; i += 4) St32(m + i, 0);
    St32(m + 0x00, 1280);        // largeur
    St32(m + 0x04, 720);         // hauteur
    St32(m + 0x08, 0);           // entrelacé
    St32(m + 0x0C, 1);           // 16:9
    St32(m + 0x10, 1);           // haute définition
    const float hz = 60.0f;
    uint32_t hzBits;
    memcpy(&hzBits, &hz, 4);
    St32(m + 0x14, hzBits);
    St32(m + 0x18, 1);           // standard vidéo : NTSC
    St32(m + 0x1C, 0x4A);
    St32(m + 0x20, 0x01);
}
WK_IMPORT(VdQueryVideoFlags) { WK_TRACE(VdQueryVideoFlags); Ret(ctx, 3); }   // 16:9 + HD
WK_IMPORT(VdGetCurrentDisplayInformation) {
    WK_TRACE(VdGetCurrentDisplayInformation);
    const uint32_t d = ctx.r3.u32;
    for (uint32_t i = 0; i < 0x58; i += 4) St32(d + i, 0);
    St16(d + 0x00, 1280);
    St16(d + 0x02, 720);
    St16(d + 0x10, 1280);
    St16(d + 0x12, 720);
}
WK_IMPORT(VdGetCurrentDisplayGamma) {
    WK_TRACE(VdGetCurrentDisplayGamma);
    const float g = 2.22222233f;
    uint32_t bits;
    memcpy(&bits, &g, 4);
    St32(ctx.r3.u32, 2);
    St32(ctx.r4.u32, bits);
}
WK_IMPORT(VdSetDisplayMode) { WK_TRACE(VdSetDisplayMode); Ret(ctx, 0); }
WK_IMPORT(VdPersistDisplay) { WK_TRACE(VdPersistDisplay); if (ctx.r4.u32) St32(ctx.r4.u32, 0); Ret(ctx, 0); }
WK_IMPORT(VdIsHSIOTrainingSucceeded) { WK_TRACE(VdIsHSIOTrainingSucceeded); Ret(ctx, 1); }
WK_IMPORT(VdRetrainEDRAM) { WK_TRACE(VdRetrainEDRAM); Ret(ctx, 0); }
WK_IMPORT(VdRetrainEDRAMWorker) { WK_TRACE(VdRetrainEDRAMWorker); Ret(ctx, 0); }
WK_IMPORT(VdEnableDisableClockGating) { WK_TRACE(VdEnableDisableClockGating); }
WK_IMPORT(VdInitializeScalerCommandBuffer) { WK_TRACE(VdInitializeScalerCommandBuffer); Ret(ctx, 0); }
WK_IMPORT(VdCallGraphicsNotificationRoutines) { WK_TRACE(VdCallGraphicsNotificationRoutines); Ret(ctx, 0); }
// Fin d'image, dans l'ordre du flux GPU (thread du processeur de commandes) : présentation native, cadence, arrêt.
namespace wk { uint32_t SwapsEmis() { return g_swaps.load(); } }

static void OnGpuSwap(uint32_t n) {
    CpSetSwapCounter(n);
#ifdef WAKFU_HAS_SHADERGEN
    wk::natif::SwapCp(n);   // présentation et capture : fil natif
#endif
    {   // cadence réelle dans le journal, toutes les 5 s (retours des joueurs)
        using clock = std::chrono::steady_clock;
        static clock::time_point t0 = clock::now();
        static uint32_t n0 = n;
        const double s = std::chrono::duration<double>(clock::now() - t0).count();
        if (s >= 5.0) {
            fprintf(stderr, "[cadence] %.1f img/s (images %u-%u)\n", (n - n0) / s, n0, n);
            // anneau = traitement des commandes (attentes mémoire comprises) ; pause demandée : 20 µs
            const double im = std::max(1u, n - n0);
            fprintf(stderr, "[cp] par image : anneau %.2f ms dont attentes mémoire %.2f ms (%.1f), pauses %.2f ms (%.1f, %.0f µs chacune)\n",
                    wk::g_cpAnneauMs / im, wk::g_cpAttenteMemMs / im, wk::g_cpAttentesMem / im, wk::g_cpPausesMs / im,
                    wk::g_cpPauses / im, wk::g_cpPauses ? 1000 * wk::g_cpPausesMs / wk::g_cpPauses : 0.0);
            wk::g_cpAnneauMs = wk::g_cpAttenteMemMs = wk::g_cpPausesMs = 0;
            wk::g_cpPauses = wk::g_cpAttentesMem = 0;
            t0 = clock::now();
            n0 = n;
        }
    }
    if (g_stopAtSwap && int(n) >= g_stopAtSwap) {
#ifdef WAKFU_HAS_SHADERGEN
        wk::natif::Vider();   // captures des dernières images écrites avant l'arrêt
#endif
        fprintf(stderr, "[arret] %u VdSwap atteints\n", n);
        if (trace::Start() && trace::g_n.load() < trace::kMax) trace::Dump();   // trace non pleine : écrite à l'arrêt
        fflush(stderr);
        _Exit(0);
    }
}

WK_IMPORT(VdSwap) {
    trace::g_frame++;
    trace::Mark('P');
    WK_TRACE(VdSwap);
    // (emplacement de 64 mots réservé par D3D, …) : l'emplacement reçoit le marqueur de swap (signature, numéro), traité
    // par le processeur de commandes.
    const uint32_t buf = ctx.r3.u32, n = ++g_swaps;
    St32(buf, 0xC0000000u | (62u << 16) | (kCpSwapOpcode << 8));
    St32(buf + 4, kCpSwapMagic);
    St32(buf + 8, n);
    Ret(ctx, 0);
}
