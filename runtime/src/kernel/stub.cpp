#include "stub.h"
#include "../process.h"
#include "sync.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace wk {
namespace {
std::mutex g_mutex;
const char* g_ring[64];
uint32_t g_ringPos = 0;
std::unordered_map<std::string, uint32_t> g_counts;
const bool g_trace = getenv("WAKFU_TRACE") && *getenv("WAKFU_TRACE") == '1';
const bool g_stopAtGpu = getenv("WAKFU_STOP_AT_GPU") && *getenv("WAKFU_STOP_AT_GPU") == '1';

void Record(const char* kind, const char* name, PPCContext& ctx, bool alwaysLog) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_ring[g_ringPos++ % 64] = name;
    uint32_t& n = g_counts[name];
    if (n == 0)   // première fois : ligne stable, exploitée par les tests (ordre d'apparition)
        fprintf(stderr, "[premier] %s %s\n", kind, name);
    if (alwaysLog || (n < 8))
        if (alwaysLog || kind[0] == 's')
            fprintf(stderr, "[%s] t%u %s r3=%08X r4=%08X r5=%08X r6=%08X LR=%08X\n", kind,
                    CurrentThread() ? CurrentThread()->threadId : 0, name, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, wk::DiagLr(ctx));
    n++;
    if (g_stopAtGpu && name[0] == 'V' && name[1] == 'd') {
        fprintf(stderr, "[arret] premier import GPU atteint : %s\n", name);
        fflush(stderr);
        _Exit(0);
    }
}
}  // namespace

void StubCall(const char* name, uint32_t, PPCContext& ctx) {
    Record("stub", name, ctx, false);
    ctx.r3.u64 = 0;
}

void Trace(const char* name, PPCContext& ctx) {
    Record("imp", name, ctx, g_trace);
    // suspension demandée par un autre thread : appliquée ici, au prochain appel noyau
    if (GuestThread* t = CurrentThread(); t && t->suspendCount > 0 && strcmp(name, "NtSuspendThread") != 0) {
        std::unique_lock<std::mutex> lock(g_kernelLock);
        g_kernelCv.wait(lock, [t] { return t->suspendCount <= 0; });
    }
}

void DumpRecentCalls() {
    fprintf(stderr, "Derniers imports appelés (du plus ancien au plus récent) :\n");
    for (uint32_t i = 0; i < 64; i++) {
        const char* n = g_ring[(g_ringPos + i) % 64];
        if (n) fprintf(stderr, "  %s\n", n);
    }
}

}  // namespace wk
