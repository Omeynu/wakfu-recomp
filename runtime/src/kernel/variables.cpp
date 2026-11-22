// Variables exportées par le noyau : le runtime alloue chaque objet en mémoire invitée et écrit
// son adresse dans le slot d'import du jeu (liste générée : kernel_vars.inc).
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstring>
#include "../process.h"

namespace wk {

namespace {
uint32_t MakeVariable(const char* name) {
    if (!strcmp(name, "XboxHardwareInfo")) {
        uint32_t a = Alloc(Heap::Kernel, 16);
        St32(a + 0, 0x00000020);   // drapeaux
        St8(a + 4, 6);             // 6 threads matériels
        return a;
    }
    if (!strcmp(name, "XboxKrnlVersion")) {
        uint32_t a = Alloc(Heap::Kernel, 8);
        St16(a + 0, 2); St16(a + 2, 0); St16(a + 4, 20353); St16(a + 6, 0);
        return a;
    }
    if (!strcmp(name, "XexExecutableModuleHandle")) return g_module.handleCell;
    if (!strcmp(name, "ExLoadedCommandLine")) {
        uint32_t a = Alloc(Heap::Kernel, 1024);
        const char* cmd = "\"default.xex\"";
        memcpy(Host(a), cmd, strlen(cmd));
        return a;
    }
    if (!strcmp(name, "KeTimeStampBundle")) {
        // +0 temps d'interruption (100 ns depuis le démarrage), +8 heure système (FILETIME), +0x10 TickCount (ms) :
        // GetTickCount du XDK lit ce champ (minuteurs XUI : un minuteur d'intervalle non nul ne se déclenchait jamais).
        const uint32_t a = Alloc(Heap::Kernel, 0x20);
        std::thread([a] {
            const auto start = std::chrono::steady_clock::now();
            for (;;) {
                const int64_t up = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start).count() / 100;
                const int64_t sys = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count() / 100 + 116444736000000000ll;
                St64(a, uint64_t(up));
                St64(a + 8, uint64_t(sys));
                St32(a + 0x10, uint32_t(up / 10000));
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }).detach();
        return a;
    }
    if (!strcmp(name, "VdGpuClockInMHz")) {
        uint32_t a = Alloc(Heap::Kernel, 4);
        St32(a, 500);
        return a;
    }
    if (!strcmp(name, "VdHSIOCalibrationLock")) return Alloc(Heap::Kernel, 0x1C);  // section critique
    if (!strcmp(name, "ExThreadObjectType")) return Alloc(Heap::Kernel, 0x40);
    // VdGlobalDevice, VdGlobalXamDevice, KeDebugMonitorData, KeCertMonitorData : cellule nulle
    return Alloc(Heap::Kernel, 16);
}

void InitModule() {
    // X_LDR_DATA_TABLE_ENTRY minimal + cellule « handle » pointée par XexExecutableModuleHandle
    uint32_t e = Alloc(Heap::Kernel, 0x100);
    St32(e + 0x18, PPC_IMAGE_BASE);                  // dll_base
    St32(e + 0x1C, PPC_IMAGE_BASE);                  // image_base
    St32(e + 0x20, PPC_IMAGE_SIZE);                  // image_size
    St32(e + 0x38, PPC_IMAGE_SIZE);                  // full_image_size
    St32(e + 0x3C, XexHeaderField(0x00010100));      // entry_point
    St32(e + 0x58, g_module.xexHeader);              // xex_header_base
    g_module.ldrEntry = e;
    g_module.handleCell = Alloc(Heap::Kernel, 4);
    St32(g_module.handleCell, e);
}
}  // namespace

void InitKernelVariables() {
    InitModule();
#define V(slot, module, ordinal, Name)                       \
    {                                                        \
        uint32_t obj = MakeVariable(#Name);                  \
        St32(slot, obj);                                     \
        fprintf(stderr, "[variable] %s -> 0x%08X\n", #Name, obj); \
    }
#include "kernel_vars.inc"
#undef V
}

}  // namespace wk
