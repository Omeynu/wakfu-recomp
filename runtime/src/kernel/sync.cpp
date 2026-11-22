#include "sync.h"
#include <chrono>
#include <cstdio>
#include <thread>
#include <unordered_map>
#include "../platform.h"
#include "../process.h"
#include "stub.h"

namespace wk {

std::mutex g_kernelLock;
std::condition_variable g_kernelCv;

thread_local uint32_t g_waitLr = 0;   // appelant de l'attente en cours (diagnostic)

namespace {
std::unordered_map<uint32_t, HandleEntry> g_handles;
uint32_t g_nextHandle = 0x00001000;

bool Signaled(uint32_t o) { return int32_t(Ld32(o + 4)) > 0; }
void Satisfy(uint32_t o) {
    switch (Ld8(o)) {
        case 1: case 2: case 9: St32(o + 4, 0); break;            // auto-réarmement / mutant
        case 5: St32(o + 4, Ld32(o + 4) - 1); break;              // sémaphore
        default: break;                                            // notification, thread
    }
}
}  // namespace

uint32_t NewHandle(HandleKind kind, uint32_t object, void* host) {
    std::lock_guard<std::mutex> lock(g_kernelLock);
    uint32_t h = g_nextHandle;
    g_nextHandle += 4;
    g_handles[h] = {kind, object, host};
    return h;
}

bool LookupHandle(uint32_t h, HandleEntry* out) {
    if (h == 0xFFFFFFFE) { *out = {HandleKind::Object, CurrentThread()->kthread, nullptr}; return true; }
    std::lock_guard<std::mutex> lock(g_kernelLock);
    auto it = g_handles.find(h);
    if (it == g_handles.end()) return false;
    *out = it->second;
    return true;
}

bool CloseHandle(uint32_t h, HandleEntry* out) {
    std::lock_guard<std::mutex> lock(g_kernelLock);
    auto it = g_handles.find(h);
    if (it == g_handles.end()) return false;
    *out = it->second;
    g_handles.erase(it);
    return true;
}

uint32_t NewDispatcher(uint8_t type, int32_t signal, uint32_t extra) {
    uint32_t o = Alloc(Heap::Kernel, 0x20);
    St8(o, type);
    St32(o + 4, uint32_t(signal));
    St32(o + 0x10, extra);
    return o;
}

void SignalLocked(uint32_t object, int32_t state) {
    St32(object + 4, uint32_t(state));
    g_kernelCv.notify_all();
}

uint32_t WaitObjects(const uint32_t* objects, uint32_t count, bool waitAll, uint32_t timeoutPtr) {
    using clock = std::chrono::steady_clock;
    bool infinite = timeoutPtr == 0;
    clock::time_point deadline = clock::now();
    if (!infinite) {
        int64_t t = int64_t(Ld64(timeoutPtr));
        if (t < 0) deadline += std::chrono::nanoseconds(-t * 100);
        else if (t > 0) {   // absolu (FILETIME) : approximé par rapport à l'horloge système
            int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count() / 100 + 116444736000000000ll;
            if (t > now) deadline += std::chrono::nanoseconds((t - now) * 100);
        }
    }
    std::unique_lock<std::mutex> lock(g_kernelLock);
    bool reported = false;
    for (;;) {
        if (waitAll) {
            bool all = true;
            for (uint32_t i = 0; i < count; i++) all &= Signaled(objects[i]);
            if (all) { for (uint32_t i = 0; i < count; i++) Satisfy(objects[i]); return STATUS_SUCCESS; }
        } else {
            for (uint32_t i = 0; i < count; i++)
                if (Signaled(objects[i])) { Satisfy(objects[i]); return STATUS_SUCCESS + i; }
        }
        if (infinite) {
            // diagnostic : attente infinie de plus de 3 s -> objets attendus (une fois)
            if (g_kernelCv.wait_for(lock, std::chrono::seconds(3)) == std::cv_status::timeout && !reported) {
                reported = true;
                fprintf(stderr, "[diag] thread %u attend depuis 3 s :", CurrentThread() ? CurrentThread()->threadId : 0);
                for (uint32_t i = 0; i < count; i++)
                    fprintf(stderr, " %08X(type %u, état %d)", objects[i], Ld8(objects[i]), int32_t(Ld32(objects[i] + 4)));
                fprintf(stderr, " LR=%08X\n", g_waitLr);
            }
        }
        else if (g_kernelCv.wait_until(lock, deadline) == std::cv_status::timeout) {
            // dernière vérification avant de renoncer
            if (!waitAll)
                for (uint32_t i = 0; i < count; i++)
                    if (Signaled(objects[i])) { Satisfy(objects[i]); return STATUS_SUCCESS + i; }
            return STATUS_TIMEOUT;
        }
    }
}

}  // namespace wk

using namespace wk;

// ---------- Événements, sémaphores ----------
WK_IMPORT(NtCreateEvent) {
    WK_TRACE(NtCreateEvent);
    // (handle*, attributs, type 0 notification / 1 synchro, état initial)
    uint32_t o = NewDispatcher(uint8_t(ctx.r5.u32 & 1), ctx.r6.u32 ? 1 : 0);
    St32(ctx.r3.u32, NewHandle(HandleKind::Object, o));
    Ret(ctx, STATUS_SUCCESS);
}
WK_IMPORT(NtCreateSemaphore) {
    WK_TRACE(NtCreateSemaphore);
    uint32_t o = NewDispatcher(5, int32_t(ctx.r5.u32), ctx.r6.u32);
    St32(ctx.r3.u32, NewHandle(HandleKind::Object, o));
    Ret(ctx, STATUS_SUCCESS);
}
static uint32_t ObjectOf(uint32_t h) {
    HandleEntry e;
    return LookupHandle(h, &e) && e.kind == HandleKind::Object ? e.object : 0;
}
WK_IMPORT(NtSetEvent) {
    WK_TRACE(NtSetEvent);
    uint32_t o = ObjectOf(ctx.r3.u32);
    if (!o) { Ret(ctx, 0xC0000008); return; }
    std::lock_guard<std::mutex> lock(g_kernelLock);
    if (ctx.r4.u32) St32(ctx.r4.u32, Ld32(o + 4));
    SignalLocked(o, 1);
    Ret(ctx, STATUS_SUCCESS);
}
WK_IMPORT(NtClearEvent) {
    WK_TRACE(NtClearEvent);
    uint32_t o = ObjectOf(ctx.r3.u32);
    if (!o) { Ret(ctx, 0xC0000008); return; }
    std::lock_guard<std::mutex> lock(g_kernelLock);
    St32(o + 4, 0);
    Ret(ctx, STATUS_SUCCESS);
}
WK_IMPORT(NtReleaseSemaphore) {
    WK_TRACE(NtReleaseSemaphore);
    uint32_t o = ObjectOf(ctx.r3.u32);
    if (!o) { Ret(ctx, 0xC0000008); return; }
    std::lock_guard<std::mutex> lock(g_kernelLock);
    int32_t prev = int32_t(Ld32(o + 4)), limit = int32_t(Ld32(o + 0x10));
    if (ctx.r5.u32) St32(ctx.r5.u32, uint32_t(prev));
    if (prev + int32_t(ctx.r4.u32) > limit) { Ret(ctx, 0xC0000047); return; }   // SEMAPHORE_LIMIT_EXCEEDED
    SignalLocked(o, prev + int32_t(ctx.r4.u32));
    Ret(ctx, STATUS_SUCCESS);
}
WK_IMPORT(KeSetEvent) {
    WK_TRACE(KeSetEvent);
    std::lock_guard<std::mutex> lock(g_kernelLock);
    uint32_t prev = Ld32(ctx.r3.u32 + 4);
    SignalLocked(ctx.r3.u32, 1);
    Ret(ctx, prev);
}
WK_IMPORT(KeResetEvent) {
    WK_TRACE(KeResetEvent);
    std::lock_guard<std::mutex> lock(g_kernelLock);
    uint32_t prev = Ld32(ctx.r3.u32 + 4);
    St32(ctx.r3.u32 + 4, 0);
    Ret(ctx, prev);
}
WK_IMPORT(NtDuplicateObject) {
    WK_TRACE(NtDuplicateObject);
    HandleEntry e;
    if (!LookupHandle(ctx.r3.u32, &e)) { Ret(ctx, 0xC0000008); return; }
    St32(ctx.r4.u32, NewHandle(e.kind, e.object, e.host));
    Ret(ctx, STATUS_SUCCESS);
}
WK_IMPORT(ObReferenceObjectByHandle) {
    WK_TRACE(ObReferenceObjectByHandle);
    HandleEntry e;
    if (!LookupHandle(ctx.r3.u32, &e)) { Ret(ctx, 0xC0000008); return; }
    if (ctx.r5.u32) St32(ctx.r5.u32, e.object);
    Ret(ctx, STATUS_SUCCESS);
}
WK_IMPORT(ObDereferenceObject) { WK_TRACE(ObDereferenceObject); }

// ---------- Attentes ----------
WK_IMPORT(KeWaitForSingleObject) {
    WK_TRACE(KeWaitForSingleObject);
    g_waitLr = wk::DiagLr(ctx);
    uint32_t o = ctx.r3.u32;
    Ret(ctx, WaitObjects(&o, 1, false, ctx.r7.u32));
}
WK_IMPORT(KeWaitForMultipleObjects) {
    WK_TRACE(KeWaitForMultipleObjects);
    g_waitLr = wk::DiagLr(ctx);
    uint32_t objs[64], n = ctx.r3.u32 > 64 ? 64 : ctx.r3.u32;
    for (uint32_t i = 0; i < n; i++) objs[i] = Ld32(ctx.r4.u32 + 4 * i);
    Ret(ctx, WaitObjects(objs, n, ctx.r5.u32 == 0, ctx.r9.u32));
}
WK_IMPORT(NtWaitForSingleObjectEx) {
    WK_TRACE(NtWaitForSingleObjectEx);
    g_waitLr = wk::DiagLr(ctx);
    uint32_t o = ObjectOf(ctx.r3.u32);
    if (!o) { Ret(ctx, 0xC0000008); return; }
    Ret(ctx, WaitObjects(&o, 1, false, ctx.r6.u32));
}
WK_IMPORT(NtWaitForMultipleObjectsEx) {
    WK_TRACE(NtWaitForMultipleObjectsEx);
    g_waitLr = wk::DiagLr(ctx);
    uint32_t objs[64], n = ctx.r3.u32 > 64 ? 64 : ctx.r3.u32;
    for (uint32_t i = 0; i < n; i++)
        if (!(objs[i] = ObjectOf(Ld32(ctx.r4.u32 + 4 * i)))) { Ret(ctx, 0xC0000008); return; }
    Ret(ctx, WaitObjects(objs, n, ctx.r5.u32 == 0, ctx.r8.u32));
}

// ---------- Sections critiques et verrous tournants ----------
// RTL_CRITICAL_SECTION : +0x10 LockCount, +0x14 RecursionCount, +0x18 OwningThread
// Diagnostic : adresse invalide -> appelant (LR) et pile hôte, une fois par adresse.
static void CheckCs(const char* fn, uint32_t cs, PPCContext& ctx) {
    if ((cs & 3) == 0 && cs >= 0x10000000) return;
    static std::mutex m;
    static std::unordered_map<uint32_t, bool> seen;
    std::lock_guard<std::mutex> lock(m);
    if (seen[cs]) return;
    seen[cs] = true;
    fprintf(stderr, "[diag] %s : section critique invalide %08X, LR=%08X, thread %u\n", fn, cs, wk::DiagLr(ctx),
            CurrentThread()->threadId);
    platform::PrintBacktrace();
}
static void CsInit(uint32_t cs) {
    St8(cs + 0, 1);
    St32(cs + 0x10, 0xFFFFFFFF);
    St32(cs + 0x14, 0);
    St32(cs + 0x18, 0);
}
WK_IMPORT(RtlInitializeCriticalSection) { WK_TRACE(RtlInitializeCriticalSection); CsInit(ctx.r3.u32); }
WK_IMPORT(RtlInitializeCriticalSectionAndSpinCount) {
    WK_TRACE(RtlInitializeCriticalSectionAndSpinCount);
    CsInit(ctx.r3.u32);
    Ret(ctx, 0);
}
WK_IMPORT(RtlEnterCriticalSection) {
    WK_TRACE(RtlEnterCriticalSection);
    CheckCs("RtlEnterCriticalSection", ctx.r3.u32, ctx);
    const uint32_t cs = ctx.r3.u32, me = CurrentThread()->kthread;
    std::unique_lock<std::mutex> lock(g_kernelLock);
    St32(cs + 0x10, Ld32(cs + 0x10) + 1);
    if (Ld32(cs + 0x18) == me) { St32(cs + 0x14, Ld32(cs + 0x14) + 1); return; }
    // Diagnostic : attente > 10 s -> section, propriétaire et attendant (une ligne par attente).
    if (!g_kernelCv.wait_for(lock, std::chrono::seconds(10), [&] { return Ld32(cs + 0x18) == 0; })) {
        fprintf(stderr, "[diag] section critique %08X attendue > 10 s : propriétaire %08X, attendant %08X (thread %u)\n",
                cs, Ld32(cs + 0x18), me, CurrentThread()->threadId);
        g_kernelCv.wait(lock, [&] { return Ld32(cs + 0x18) == 0; });
    }
    St32(cs + 0x18, me);
    St32(cs + 0x14, 1);
}
WK_IMPORT(RtlTryEnterCriticalSection) {
    WK_TRACE(RtlTryEnterCriticalSection);
    const uint32_t cs = ctx.r3.u32, me = CurrentThread()->kthread;
    std::lock_guard<std::mutex> lock(g_kernelLock);
    const uint32_t owner = Ld32(cs + 0x18);
    if (owner && owner != me) { Ret(ctx, 0); return; }
    St32(cs + 0x10, Ld32(cs + 0x10) + 1);
    St32(cs + 0x18, me);
    St32(cs + 0x14, Ld32(cs + 0x14) + 1);
    Ret(ctx, 1);
}
WK_IMPORT(RtlLeaveCriticalSection) {
    WK_TRACE(RtlLeaveCriticalSection);
    CheckCs("RtlLeaveCriticalSection", ctx.r3.u32, ctx);
    const uint32_t cs = ctx.r3.u32;
    std::lock_guard<std::mutex> lock(g_kernelLock);
    St32(cs + 0x10, Ld32(cs + 0x10) - 1);
    const uint32_t rec = Ld32(cs + 0x14) - 1;
    St32(cs + 0x14, rec);
    if (rec == 0) { St32(cs + 0x18, 0); g_kernelCv.notify_all(); }
}
// Verrou tournant : mot invité = propriétaire (KTHREAD) ou 0.
static void SpinAcquire(uint32_t lockAddr) {
    const uint32_t me = CurrentThread()->kthread;
    std::unique_lock<std::mutex> lock(g_kernelLock);
    g_kernelCv.wait(lock, [&] { uint32_t o = Ld32(lockAddr); return o == 0 || o == me; });
    St32(lockAddr, me);
}
static void SpinRelease(uint32_t lockAddr) {
    std::lock_guard<std::mutex> lock(g_kernelLock);
    St32(lockAddr, 0);
    g_kernelCv.notify_all();
}
WK_IMPORT(KfAcquireSpinLock) { WK_TRACE(KfAcquireSpinLock); SpinAcquire(ctx.r3.u32); Ret(ctx, 0); }
WK_IMPORT(KfReleaseSpinLock) { WK_TRACE(KfReleaseSpinLock); SpinRelease(ctx.r3.u32); }
WK_IMPORT(KeAcquireSpinLockAtRaisedIrql) { WK_TRACE(KeAcquireSpinLockAtRaisedIrql); SpinAcquire(ctx.r3.u32); }
WK_IMPORT(KeReleaseSpinLockFromRaisedIrql) { WK_TRACE(KeReleaseSpinLockFromRaisedIrql); SpinRelease(ctx.r3.u32); }
