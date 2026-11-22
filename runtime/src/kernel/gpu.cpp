// GPU minimal du jalon 2 (Vd*) : l'anneau de commandes est « consommé » instantanément (pointeur de
// lecture = pointeur d'écriture), le rappel d'interruption est appelé à 60 Hz. Les paquets PM4 ne
// sont pas exécutés : c'est le jalon 3.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <filesystem>
#include "../platform.h"
#include "../trace.h"
#include "../process.h"
#include "stub.h"
#include "sync.h"
#include "gpu_cp.h"
#include "frontbuffer.h"
#include "window.h"
#ifdef WAKFU_HAS_SHADERGEN
#include "../gpu/renderer.h"
#include "../gpu/shader_cache.h"
#endif
#include <set>
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

static void OnGpuSwap(uint32_t n, const uint32_t fetch[6]);
static void FinishFrame(uint32_t n, const uint32_t fetch[6]);

namespace wk {
extern std::string g_gameDir, g_dataDir;
void StartDpcThread();

void StartSystemThreads() {
    StartDpcThread();
    CpLoadShaderCatalog(g_gameDir + "/Media/Shaders");
    CpLoadShaderCatalogFromImage(PPC_IMAGE_BASE, PPC_IMAGE_SIZE);
#ifdef WAKFU_HAS_SHADERGEN
    wk::gpu::ShaderCacheSetDir(g_dataDir + "/cache");   // SPIR-V et pipelines Vulkan sur disque
    CpSetShaderHook(wk::gpu::ShaderCacheOnLoad);   // traduction des shaders au premier chargement
    CpPrecompileCatalog(wk::gpu::ShaderCacheOnLoad);   // et d'avance pour ceux du jeu
#endif
    // Thread « processeur de commandes » : exécute l'anneau, interruptions CP (source 1). Il porte aussi
    // la fenêtre (création, événements, présentation) et le rendu, initialisés avant de rendre la main.
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
            if (!wk::gpu::RendererInit(&err)) fprintf(stderr, "[rendu] désactivé : %s\n", err.c_str());
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
                if (wptr != rptr) { trace::Mark('K', wptr); rptr = CpProcessRing(PhysToGuest(g_ringBase), size, rptr, wptr); trace::Mark('Q', rptr); }
                if (g_rptrWriteback) St32(PhysToGuest(g_rptrWriteback), rptr);
            }
            window::Pump();
#ifdef WAKFU_HAS_SHADERGEN
            // lots GPU en vol : attente bloquante de leur fin (écritures différées appliquées dès qu'elle arrive)
            if (wk::gpu::RendererWait(500000)) continue;
#endif
            // Anneau vide, rien en vol : le jeu attend souvent une barrière que le CP doit lire au plus vite (BlockOnFence,
            // ≈ 1 image d'attente par image). 500 µs -> 20 µs : 17,6 -> 20,4-20,9 img/s (sauvegarde du joueur, zone 1).
            std::this_thread::sleep_for(std::chrono::microseconds(20));
        }
    }).detach();
    while (!cpReady.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    // Thread « vblank » : 60 Hz, indépendant du processeur de commandes (source 0).
    GuestThread* vb = CreateGuestThread(0x10000);
    std::thread([vb] {
        PPCContext ctx{};
        AttachThread(vb, ctx);
        auto next = std::chrono::steady_clock::now();
        uint32_t vblank = 0;
        for (;;) {
            next += std::chrono::microseconds(16667);
            std::this_thread::sleep_until(next);
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
// Captures : WAKFU_CAPTURE="60,300,900" (numéros d'image) ou "toutes:N" ; dossier WAKFU_CAPTURE_DIR (captures/).
namespace {
bool WantCapture(uint32_t n) {
    static const std::string spec = getenv("WAKFU_CAPTURE") ? getenv("WAKFU_CAPTURE") : "";
    if (spec.empty()) return false;
    if (spec.rfind("toutes:", 0) == 0) { const int k = atoi(spec.c_str() + 7); return k > 0 && n % k == 0; }
    static std::set<uint32_t> frames = [] {
        std::set<uint32_t> f;
        for (size_t i = 0; i < spec.size();) {
            size_t j = spec.find(',', i);
            if (j == std::string::npos) j = spec.size();
            f.insert(uint32_t(atoi(spec.substr(i, j - i).c_str())));
            i = j + 1;
        }
        return f;
    }();
    return frames.count(n) > 0;
}
}  // namespace

// Fin d'image, dans l'ordre du flux GPU (thread du processeur de commandes) : rendu, statistiques,
// capture (après les resolves de l'image), arrêt éventuel.
static void OnGpuSwap(uint32_t n, const uint32_t fetch[6]) {
    CpSetSwapCounter(n);
    {   // cadence réelle dans le journal, toutes les 5 s (retours des joueurs)
        using clock = std::chrono::steady_clock;
        static clock::time_point t0 = clock::now();
        static uint32_t n0 = n;
        const double s = std::chrono::duration<double>(clock::now() - t0).count();
        if (s >= 5.0) {
            fprintf(stderr, "[cadence] %.1f img/s (images %u-%u)\n", (n - n0) / s, n0, n);
            t0 = clock::now();
            n0 = n;
        }
    }
    const CpFrameStats st = CpEndFrame();
#ifdef WAKFU_HAS_SHADERGEN
    const wk::gpu::RendererFrameStats rs = wk::gpu::RendererEndFrame();
    static uint64_t faultsBefore = 0;
    const uint64_t faults = platform::WatchFaults() - faultsBefore;
    faultsBefore += faults;
    if (wk::gpu::RendererActive() && (n <= 3 || n % 60 == 0))
        fprintf(stderr, "[rendu %u] draws %u (ignorés %u), resolves %u (ignorés %u), effacements %u, textures %u (échecs %u), lots %u (attentes %u : chevauchement %u, CP %u, lots en vol %u), GPU %.1f ms, fautes %llu\n", n,
                rs.drawn, rs.skipped, rs.resolves, rs.resolvesSkipped, rs.clears, rs.textureUploads, rs.textureFailures,
                rs.batches, rs.waits, rs.waitsOverlap, rs.waitsSync, rs.waitsRing, rs.gpuUs / 1000.0, (unsigned long long)faults);
    if (wk::gpu::RendererActive() && rs.gpuCatUs[0] + rs.gpuCatUs[5] && (n % 60 == 0))
        fprintf(stderr, "[gpu %u] draws %.1f ms, décodages %.1f, envois %.1f, resolves %.1f, effacements %.1f, reste %.1f\n", n,
                rs.gpuCatUs[0] / 1000.0, rs.gpuCatUs[1] / 1000.0, rs.gpuCatUs[2] / 1000.0, rs.gpuCatUs[3] / 1000.0,
                rs.gpuCatUs[4] / 1000.0, rs.gpuCatUs[5] / 1000.0);
#endif
    if (n <= 3 || n % 60 == 0) {
        fprintf(stderr, "[image %u] draws %u (indices %u, primitives %s) resolves %u, shaders chargés %u "
                        "(%u distincts, %u hors catalogue) ; cumul : %u shaders (%u du XEX, %u hors catalogue), %llu resolves\n",
                n, st.draws, st.indices, st.prims.c_str(), st.resolves, st.shaderLoads, st.shaders, st.unknownShaders,
                st.totalShaders, st.totalXexShaders, st.totalUnknownShaders, (unsigned long long)st.totalResolves);
        if (!st.unknownOps.empty()) fprintf(stderr, "[gpu] paquets type 3 inconnus (cumul) : %s\n", st.unknownOps.c_str());
    }
    if (n == 1) {
        const FrontBuffer fb = ParseFetchWords(fetch);
        fprintf(stderr, "[gpu] tampon d'image : adresse %08X %ux%u pas %u format %u octets %u tuilé %u ; "
                        "dernier resolve vers %08X (pas %08X info %08X)\n",
                fb.address, fb.width, fb.height, fb.pitch, fb.format, fb.endian, fb.tiled,
                st.lastResolve.destBase, st.lastResolve.destPitch, st.lastResolve.destInfo);
    }
    // Présentation, capture et arrêt lisent le tampon d'image : à la fin réelle du travail GPU de l'image
    // (rendu asynchrone), dans l'ordre du flux ; tout de suite sans rendu.
    std::array<uint32_t, 6> f;
    std::copy(fetch, fetch + 6, f.begin());
    auto finish = [n, f] { FinishFrame(n, f.data()); };
#ifdef WAKFU_HAS_SHADERGEN
    if (wk::gpu::RendererActive() && wk::gpu::RendererDefer(finish)) return;
#endif
    finish();
}

static void FinishFrame(uint32_t n, const uint32_t fetch[6]) {
    window::Present(fetch);
    if (WantCapture(n)) {
        static const std::string dir = getenv("WAKFU_CAPTURE_DIR") ? getenv("WAKFU_CAPTURE_DIR") : "captures";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::string path = dir + "/image_" + std::to_string(n) + ".png";
        fprintf(stderr, "[capture] %s : %s\n", path.c_str(), CaptureFrontBuffer(fetch, path) ? "écrite" : "échec");
    }
    if (g_stopAtSwap && int(n) >= g_stopAtSwap) {
        fprintf(stderr, "[arret] %u VdSwap atteints\n", n);
        fflush(stderr);
        _Exit(0);
    }
}

WK_IMPORT(VdSwap) {
    trace::g_frame++;
    trace::Mark('P');
    WK_TRACE(VdSwap);
    // (emplacement de 64 mots réservé par D3D, constante de texture du tampon d'image, …) : l'emplacement
    // reçoit le marqueur de swap (signature, numéro, constante recopiée), traité par le processeur de commandes.
    const uint32_t buf = ctx.r3.u32, n = ++g_swaps;
    St32(buf, 0xC0000000u | (62u << 16) | (kCpSwapOpcode << 8));
    St32(buf + 4, kCpSwapMagic);
    St32(buf + 8, n);
    for (uint32_t i = 0; i < 6; i++) St32(buf + 12 + 4 * i, Ld32(ctx.r4.u32 + 4 * i));
    Ret(ctx, 0);
}
