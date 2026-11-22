#include "process.h"
#include <atomic>
#include "platform.h"
#include <cstdlib>
#include <thread>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace wk {

Module g_module;

uint32_t XexHeaderField(uint32_t key, bool* found) {
    const uint32_t h = g_module.xexHeader;
    const uint32_t count = Ld32(h + 0x14);
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t k = Ld32(h + 0x18 + 8 * i), v = Ld32(h + 0x18 + 8 * i + 4);
        if (k != key) continue;
        if (found) *found = true;
        switch (k & 0xFF) {
            case 0x00: return v;                       // valeur en ligne
            case 0x01: return h + 0x18 + 8 * i + 4;    // adresse de la valeur (1 mot)
            default: return h + v;                     // adresse des données
        }
    }
    if (found) *found = false;
    return 0;
}

namespace {
thread_local GuestThread* t_current = nullptr;
std::atomic<uint32_t> g_nextThreadId{1};
std::mutex g_threadsMutex;
std::unordered_map<uint32_t, GuestThread*> g_threads;   // KTHREAD invité -> thread
}

GuestThread* FindThreadByKthread(uint32_t kthread) {
    std::lock_guard<std::mutex> lock(g_threadsMutex);
    auto it = g_threads.find(kthread);
    return it == g_threads.end() ? nullptr : it->second;
}

GuestThread* CurrentThread() { return t_current; }

GuestThread* CreateGuestThread(uint32_t stackSize) {
    auto* t = new GuestThread();
    stackSize = (stackSize + 0xFFFF) & ~0xFFFFu;
    t->stackSize = stackSize;
    t->stackBase = Alloc(Heap::Stack, stackSize, 0x10000);
    t->pcr = Alloc(Heap::Kernel, 0x1000, 0x1000);
    t->kthread = Alloc(Heap::Kernel, 0x400, 0x10);
    t->threadId = g_nextThreadId++;
    const uint32_t top = t->stackBase + stackSize;
    // PCR (champs lus par le jeu : +0x100 thread courant, +0x10C CPU, +0x150 DPC actif)
    St32(t->pcr + 0x70, top);
    St32(t->pcr + 0x74, t->stackBase);
    St32(t->pcr + 0x100, t->kthread);
    St8(t->pcr + 0x10C, uint8_t(t->threadId % 6));
    St8(t->kthread + 0, 6);                  // en-tête dispatcher : type thread, non signalé
    St32(t->kthread + 0x5C, top);
    St32(t->kthread + 0x60, t->stackBase);
    St32(t->kthread + 0x14C, t->threadId);
    std::lock_guard<std::mutex> lock(g_threadsMutex);
    g_threads[t->kthread] = t;
    return t;
}

void AttachThread(GuestThread* t, PPCContext& ctx) {
    ctx.fpscr.loadFromHost();
    ctx.r1.u64 = t->stackBase + t->stackSize - 0x100;
    ctx.r13.u64 = t->pcr;
    platform::RegisterCurrentThreadForDump(t->threadId);
    t_current = t;
}

void StartWatchdog() {
    if (const char* e = getenv("WAKFU_DUMP_AFTER")) platform::DumpAllThreadsAfter(atoi(e));
}

bool CallGuest(uint32_t address, PPCContext& ctx) {
    if (address < PPC_CODE_BASE || address >= PPC_CODE_BASE + PPC_CODE_SIZE) return false;
    PPCFunc* f = PPC_LOOKUP_FUNC(g_base, address);
    if (!f) return false;
    f(ctx, g_base);
    return true;
}

}  // namespace wk

// Appel indirect vérifié (cf. indirect.h) : cible hors code ou absente de la table -> journal, r3 = 0.
void WkCallIndirect(PPCContext& ctx, uint8_t* base, uint32_t target) {
    if (target >= PPC_CODE_BASE && target < PPC_CODE_BASE + PPC_CODE_SIZE) {
        if (PPCFunc* f = PPC_LOOKUP_FUNC(base, target)) { f(ctx, base); return; }
    }
    static std::mutex m;
    static std::unordered_map<uint32_t, uint32_t> seen;
    std::lock_guard<std::mutex> lock(m);
    if (seen[target]++ < 3)
        fprintf(stderr, "[diag] appel indirect vers une cible inconnue %08X (LR=%08X, thread %u)\n", target,
                wk::DiagLr(ctx), wk::CurrentThread() ? wk::CurrentThread()->threadId : 0);
    ctx.r3.u64 = 0;
}
