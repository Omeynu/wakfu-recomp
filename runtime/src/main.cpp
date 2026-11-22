// Hôte du jalon 1 : charge l'image recompilée d'Islands of Wakfu et exécute _xstart.
// Usage : wakfu <image.bin> <default.xex patché> <dossier du jeu> [dossier de la TU] [dossier de données]
#include "config.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>
#include "kernel/xma.h"
#include "platform.h"
#include "kernel/stub.h"
#include "indirect.h"
#include "process.h"
#include "quit.h"
#include <filesystem>
#include <string>
#ifdef WAKFU_HAS_SHADERGEN
#include "gpu/renderer.h"
#endif

extern PPCFuncMapping PPCFuncMappings[];
PPC_EXTERN_FUNC(_xstart);

namespace wk {
void InitKernelVariables();   // kernel/variables.cpp
void StartSystemThreads();    // kernel/gpu.cpp, kernel/threads.cpp
std::string g_gameDir;
extern std::string g_overlayDir, g_dataDir;   // kernel/files.cpp
}

namespace wk {
[[noreturn]] void Quit(int code, const char* reason, bool saveCaches) {
    fprintf(stderr, "[sortie] code %d : %s\n", code, reason);
#ifdef WAKFU_HAS_SHADERGEN
    if (saveCaches) gpu::RendererSavePipelineCache();
#else
    (void)saveCaches;
#endif
    fflush(stdout);
    fflush(stderr);
    _Exit(code);
}
}  // namespace wk

namespace {

// Journal des 3 derniers lancements : journal.txt (en cours), journal.1.txt, journal.2.txt.
void RotateJournals(const std::string& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path d = fs::u8path(dir);
    fs::remove(d / "journal.2.txt", ec);
    fs::rename(d / "journal.1.txt", d / "journal.2.txt", ec);
    fs::rename(d / "journal.txt", d / "journal.1.txt", ec);
}

std::vector<uint8_t> ReadFile(const char* p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

void OnCrash(int code, uintptr_t a) {
    const uintptr_t b = uintptr_t(wk::g_base);
    fprintf(stderr, "\n[plantage] code %d, adresse hôte %p", code, (void*)a);
    if (a >= b && a < b + 0x100000000ull) fprintf(stderr, " (invitée 0x%08lX)", (unsigned long)(a - b));
    fprintf(stderr, "\n");
    wk::DumpRecentCalls();
    wk::platform::PrintBacktrace();
    wk::Quit(wk::kExitCrash, "plantage (voir [plantage] plus haut)", false);
}

}  // namespace

int main(int argc, char** argv) {
    wk::platform::Init();
    if (argc < 3) {
        fprintf(stderr, "usage : %s <image.bin> <default.xex patché> [dossier du jeu]\n", argv[0]);
        wk::Quit(wk::kExitUsage, "arguments manquants", false);
    }
    if (argc > 3) wk::g_gameDir = argv[3];
    if (argc > 4) wk::g_overlayDir = argv[4];
    if (argc > 5) wk::g_dataDir = argv[5];
    wk::config::Load(wk::g_dataDir);
    if (!wk::config::g.console) {   // option [confort] console
        RotateJournals(wk::g_dataDir);
        wk::platform::DetachConsole(wk::g_dataDir + "/journal.txt");
    }

    wk::platform::InstallCrashHandler(OnCrash);

    // 4 Go d'espace invité ; la table PPC_LOOKUP_FUNC tombe dedans (après l'image).
    void* mem = wk::platform::ReserveGuestMemory(0x100000000ull);
    if (!mem) wk::Quit(wk::kExitMemory, "réservation des 4 Go de mémoire invitée impossible", false);
    wk::g_base = static_cast<uint8_t*>(mem);
    wk::platform::WatchInit(wk::g_base);

    auto image = ReadFile(argv[1]);
    if (image.size() != PPC_IMAGE_SIZE) {
        fprintf(stderr, "image : %zu octets, attendu %llu\n", image.size(), (unsigned long long)PPC_IMAGE_SIZE);
        wk::Quit(wk::kExitGameFiles, "image.bin absente ou de mauvaise taille (recompiler le jeu)", false);
    }
    memcpy(wk::g_base + PPC_IMAGE_BASE, image.data(), image.size());

    auto xex = ReadFile(argv[2]);
    if (xex.size() < 0x18 || memcmp(xex.data(), "XEX2", 4) != 0) wk::Quit(wk::kExitGameFiles, "default.xex absent ou invalide (recompiler le jeu)", false);
    uint32_t headerSize = __builtin_bswap32(*reinterpret_cast<uint32_t*>(xex.data() + 8));
    wk::g_module.xexHeaderSize = headerSize;
    wk::g_module.xexHeader = wk::Alloc(wk::Heap::Kernel, headerSize, 0x1000);
    memcpy(wk::Host(wk::g_module.xexHeader), xex.data(), headerSize);

    size_t n = 0;
    for (auto* m = PPCFuncMappings; m->host; m++, n++) PPC_LOOKUP_FUNC(wk::g_base, m->guest) = m->host;
    fprintf(stderr, "[demarrage] %zu fonctions dans la table, image 0x%08llX+0x%llX\n", n,
            (unsigned long long)PPC_IMAGE_BASE, (unsigned long long)PPC_IMAGE_SIZE);

    wk::InitKernelVariables();
    wk::xma::Init();

    PPCContext ctx{};
    bool found = false;
    uint32_t stack = wk::XexHeaderField(0x00020200, &found);
    wk::AttachThread(wk::CreateGuestThread(found && stack ? stack : 0x40000), ctx);
    wk::StartSystemThreads();
    wk::StartWatchdog();
    _xstart(ctx, wk::g_base);
    fprintf(stderr, "[fin] _xstart a retourné r3=%08X\n", ctx.r3.u32);
    return 0;
}
