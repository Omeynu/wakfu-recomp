// Threads invités sur threads hôtes, DPC, délais.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include "../process.h"
#include "stub.h"
#include "sync.h"

using namespace wk;

namespace {
struct ThreadExit { uint32_t code; };   // levée par ExTerminateThread, rattrapée au sommet du thread

void ThreadMain(GuestThread* t, uint32_t xapiStartup, uint32_t start, uint32_t context) {
    PPCContext ctx{};
    AttachThread(t, ctx);
    {
        std::unique_lock<std::mutex> lock(g_kernelLock);
        g_kernelCv.wait(lock, [&] { return t->suspendCount <= 0; });
    }
    uint32_t code = 0;
    try {
        bool ok;
        if (xapiStartup) { ctx.r3.u64 = start; ctx.r4.u64 = context; ok = CallGuest(xapiStartup, ctx); }
        else { ctx.r3.u64 = context; ok = CallGuest(start, ctx); }
        if (!ok) fprintf(stderr, "[thread %u] point d'entrée inconnu %08X\n", t->threadId, xapiStartup ? xapiStartup : start);
        code = ctx.r3.u32;
    } catch (const ThreadExit& e) {
        code = e.code;
    }
    fprintf(stderr, "[thread %u] fin, code %08X\n", t->threadId, code);
    std::lock_guard<std::mutex> lock(g_kernelLock);
    St32(t->kthread + 0x18, code);
    SignalLocked(t->kthread, 1);
}

// DPC : file traitée par un thread invité dédié.
// KDPC : +0x0C routine, +0x10 contexte, +0x14 arg1, +0x18 arg2
std::deque<uint32_t> g_dpcQueue;
}  // namespace

namespace wk {
void StartDpcThread() {
    GuestThread* t = CreateGuestThread(0x10000);
    std::thread([t] {
        PPCContext ctx{};
        AttachThread(t, ctx);
        for (;;) {
            uint32_t dpc;
            {
                std::unique_lock<std::mutex> lock(g_kernelLock);
                g_kernelCv.wait(lock, [] { return !g_dpcQueue.empty(); });
                dpc = g_dpcQueue.front();
                g_dpcQueue.pop_front();
            }
            ctx.r3.u64 = dpc;
            ctx.r4.u64 = Ld32(dpc + 0x10);
            ctx.r5.u64 = Ld32(dpc + 0x14);
            ctx.r6.u64 = Ld32(dpc + 0x18);
            St32(t->pcr + 0x150, 1);       // DPC actif
            CallGuest(Ld32(dpc + 0x0C), ctx);
            St32(t->pcr + 0x150, 0);
        }
    }).detach();
}
}  // namespace wk

WK_IMPORT(ExCreateThread) {
    WK_TRACE(ExCreateThread);
    // (handle*, taille de pile, id*, xapi_startup, start, contexte, drapeaux)
    const uint32_t flags = ctx.r9.u32;
    GuestThread* t = CreateGuestThread(ctx.r4.u32 ? ctx.r4.u32 : 0x10000);
    t->suspendCount = (flags & 1) ? 1 : 0;   // CREATE_SUSPENDED
    if (ctx.r3.u32) St32(ctx.r3.u32, NewHandle(HandleKind::Object, t->kthread));
    if (ctx.r5.u32) St32(ctx.r5.u32, t->threadId);
    fprintf(stderr, "[thread %u] création : entrée %08X (xapi %08X) contexte %08X drapeaux %08X\n",
            t->threadId, ctx.r7.u32, ctx.r6.u32, ctx.r8.u32, flags);
    std::thread(ThreadMain, t, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32).detach();
    Ret(ctx, STATUS_SUCCESS);
}
WK_IMPORT(ExTerminateThread) { WK_TRACE(ExTerminateThread); throw ThreadExit{ctx.r3.u32}; }
static GuestThread* ThreadFromHandle(uint32_t h) {
    HandleEntry e;
    if (!LookupHandle(h, &e)) return nullptr;
    return FindThreadByKthread(e.object);
}
WK_IMPORT(NtResumeThread) {
    WK_TRACE(NtResumeThread);
    GuestThread* t = ThreadFromHandle(ctx.r3.u32);
    if (!t) { Ret(ctx, 0xC0000008); return; }
    std::lock_guard<std::mutex> lock(g_kernelLock);
    if (ctx.r4.u32) St32(ctx.r4.u32, uint32_t(t->suspendCount));
    if (t->suspendCount > 0) t->suspendCount--;
    else if (t != CurrentThread()) t->resumeCredit = true;   // cf. NtSuspendThread
    g_kernelCv.notify_all();
    Ret(ctx, STATUS_SUCCESS);
}
WK_IMPORT(NtSuspendThread) {
    WK_TRACE(NtSuspendThread);
    // Suspension de soi-même : bloque réellement jusqu'à NtResumeThread. Suspension d'un autre thread :
    // effective à son prochain appel noyau (pas de préemption d'un thread hôte en cours d'exécution).
    GuestThread* t = ThreadFromHandle(ctx.r3.u32);
    if (!t) { Ret(ctx, 0xC0000008); return; }
    std::unique_lock<std::mutex> lock(g_kernelLock);
    if (ctx.r4.u32) St32(ctx.r4.u32, uint32_t(t->suspendCount));
    // Réveil anticipé : des threads du jeu signalent qu'ils deviennent inactifs puis se suspendent ; si le
    // NtResumeThread du contrôleur tombe entre les deux (fenêtre minuscule sur la console, large ici), il serait
    // perdu et le thread dormirait pour toujours. Un crédit (un seul) annule alors la prochaine auto-suspension.
    if (t == CurrentThread() && t->resumeCredit) {
        t->resumeCredit = false;
        Ret(ctx, STATUS_SUCCESS);
        return;
    }
    t->suspendCount++;
    if (t == CurrentThread()) {
        // Auto-suspension bornée à 20 ms : le lecteur vidéo du jeu (XUI) perd parfois la reprise que son thread
        // principal devait envoyer (ordonnancement hôte différent de la console) et l'intro se figeait. Ses
        // threads revérifient leur état à chaque tour et se resuspendent s'il n'y a rien à faire.
        if (!g_kernelCv.wait_for(lock, std::chrono::milliseconds(20), [t] { return t->suspendCount <= 0; }))
            t->suspendCount--;
    }
    Ret(ctx, STATUS_SUCCESS);
}
WK_IMPORT(KeSetAffinityThread) { WK_TRACE(KeSetAffinityThread); Ret(ctx, 0); }
WK_IMPORT(KeSetBasePriorityThread) { WK_TRACE(KeSetBasePriorityThread); Ret(ctx, 0); }
WK_IMPORT(KeQueryBasePriorityThread) { WK_TRACE(KeQueryBasePriorityThread); Ret(ctx, 0); }
WK_IMPORT(KeDelayExecutionThread) {
    WK_TRACE(KeDelayExecutionThread);
    // (mode, alertable, LARGE_INTEGER* intervalle) ; négatif = relatif en 100 ns
    const int64_t t = ctx.r5.u32 ? int64_t(Ld64(ctx.r5.u32)) : 0;
    if (t < 0) std::this_thread::sleep_for(std::chrono::nanoseconds(-t * 100));
    else std::this_thread::yield();
    Ret(ctx, STATUS_SUCCESS);
}
WK_IMPORT(KeInitializeDpc) {
    WK_TRACE(KeInitializeDpc);
    const uint32_t d = ctx.r3.u32;
    St16(d, 19);   // type DPC
    St32(d + 0x0C, ctx.r4.u32);
    St32(d + 0x10, ctx.r5.u32);
}
WK_IMPORT(KeInsertQueueDpc) {
    WK_TRACE(KeInsertQueueDpc);
    const uint32_t d = ctx.r3.u32;
    St32(d + 0x14, ctx.r4.u32);
    St32(d + 0x18, ctx.r5.u32);
    std::lock_guard<std::mutex> lock(g_kernelLock);
    g_dpcQueue.push_back(d);
    g_kernelCv.notify_all();
    Ret(ctx, 1);
}
WK_IMPORT(KiApcNormalRoutineNop) { WK_TRACE(KiApcNormalRoutineNop); }
