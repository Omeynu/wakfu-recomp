#include "platform.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <csignal>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>
#endif

namespace wk::platform {

namespace {
constexpr uint32_t kWatchPages = 0x20000000u >> 12;   // pages de 4 Ko de la mémoire physique
uint8_t* g_watchBase = nullptr;
std::atomic<uint64_t> g_watchGen{1};
std::atomic<uint64_t> g_watchFaults{0};
std::atomic<uint64_t> g_pageGen[kWatchPages];
std::atomic<uint8_t> g_pageRo[kWatchPages];           // 1 : page en lecture seule (dans les deux vues)
bool SetPages(uint32_t first, uint32_t count, bool ro);
}  // namespace

void WatchInit(uint8_t* guestBase) { g_watchBase = guestBase; }
bool WatchFault(uintptr_t addr) {
    if (!g_watchBase) return false;
    const uintptr_t off = addr - uintptr_t(g_watchBase);
    if (off < 0xA0000000u || off >= 0xE0000000u) return false;
    const uint32_t page = uint32_t(off & 0x1FFFFFFFu) >> 12;
    if (!g_pageRo[page].load()) return false;
    g_pageGen[page].store(g_watchGen.fetch_add(1) + 1);   // avant l'écriture : un lecteur la verra comme modifiée
    SetPages(page, 1, false);
    g_pageRo[page].store(0);
    g_watchFaults++;
    return true;
}
uint64_t WatchFaults() { return g_watchFaults.load(); }
uint64_t WatchSerial() { return g_watchGen.load(); }
void WatchProtect(uint32_t physStart, uint32_t physEnd) {
    if (!g_watchBase || physEnd <= physStart) return;
    const uint32_t first = (physStart & 0x1FFFFFFFu) >> 12, last = std::min(kWatchPages, ((physEnd - 1) & 0x1FFFFFFFu) / 4096 + 1);
    for (uint32_t p = first; p < last;) {   // suites de pages encore inscriptibles
        if (g_pageRo[p].load()) { p++; continue; }
        uint32_t q = p;
        while (q < last && !g_pageRo[q].load()) g_pageRo[q++].store(1);
        if (!SetPages(p, q - p, true)) for (uint32_t k = p; k < q; k++) g_pageRo[k].store(0);
        p = q;
    }
}
uint64_t WatchGen(uint32_t physStart, uint32_t physEnd) {
    if (physEnd <= physStart) return 0;
    const uint32_t first = (physStart & 0x1FFFFFFFu) >> 12, last = std::min(kWatchPages, ((physEnd - 1) & 0x1FFFFFFFu) / 4096 + 1);
    uint64_t g = 0;
    for (uint32_t p = first; p < last; p++) g = std::max(g, g_pageGen[p].load());
    return g;
}
void WatchTouch(uint32_t physStart, uint32_t physEnd, bool writable) {
    if (physEnd <= physStart) return;
    const uint32_t first = (physStart & 0x1FFFFFFFu) >> 12, last = std::min(kWatchPages, ((physEnd - 1) & 0x1FFFFFFFu) / 4096 + 1);
    const uint64_t g = g_watchGen.fetch_add(1) + 1;
    for (uint32_t p = first; p < last; p++) g_pageGen[p].store(g);
    if (!writable) return;
    for (uint32_t p = first; p < last;) {   // suites de pages en lecture seule : une protection par suite
        if (!g_pageRo[p].load()) { p++; continue; }
        uint32_t q = p;
        while (q < last && g_pageRo[q].load()) g_pageRo[q++].store(0);
        SetPages(p, q - p, false);
        p = q;
    }
}

namespace { CrashCallback g_onCrash = nullptr; }

#ifdef _WIN32

void Init() {
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
}

// Mémoire invitée : 4 Go ; la mémoire physique (512 Mo) est vue en 0xA0000000 et en miroir en 0xC0000000
// (pages de 16 Mo sur la console). Réservations « placeholder » (Windows 10 1803+), API chargées à
// l'exécution ; repli sans miroir si elles manquent.
void* ReserveGuestMemory(uint64_t size) {
    using VA2 = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
    using MVF3 = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void*, ULONG);
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    auto va2 = kb ? VA2(GetProcAddress(kb, "VirtualAlloc2")) : nullptr;
    auto mvf3 = kb ? MVF3(GetProcAddress(kb, "MapViewOfFile3")) : nullptr;
    constexpr uint64_t kPhys = 0xA0000000ull, kMirror = 0xC0000000ull, kSize = 0x20000000ull;
    if (va2 && mvf3 && size == 0x100000000ull) {
        HANDLE sec = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, DWORD(kSize), nullptr);
        char* p = sec ? (char*)va2(nullptr, nullptr, SIZE_T(size), MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                   PAGE_NOACCESS, nullptr, 0) : nullptr;
        if (p) {
            // Découpe : [0, A0) [A0, C0) [C0, E0) [E0, fin)
            bool ok = VirtualFree(p, SIZE_T(kPhys), MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) &&
                      VirtualFree(p + kPhys, SIZE_T(kSize), MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) &&
                      VirtualFree(p + kMirror, SIZE_T(kSize), MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
            ok = ok && mvf3(sec, GetCurrentProcess(), p + kPhys, 0, SIZE_T(kSize), MEM_REPLACE_PLACEHOLDER,
                            PAGE_READWRITE, nullptr, 0);
            ok = ok && mvf3(sec, GetCurrentProcess(), p + kMirror, 0, SIZE_T(kSize), MEM_REPLACE_PLACEHOLDER,
                            PAGE_READWRITE, nullptr, 0);
            ok = ok && va2(nullptr, p, SIZE_T(kPhys), MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
                           PAGE_READWRITE, nullptr, 0);
            ok = ok && va2(nullptr, p + 0xE0000000ull, SIZE_T(size - 0xE0000000ull),
                           MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
            if (ok) return p;
            fprintf(stderr, "[memoire] miroir physique impossible (%lu), repli sans miroir\n", GetLastError());
        }
    }
    return VirtualAlloc(nullptr, SIZE_T(size), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

namespace {
bool SetPages(uint32_t first, uint32_t count, bool ro) {
    DWORD old;
    bool ok = true;
    for (uint64_t view : {0xA0000000ull, 0xC0000000ull})
        ok = VirtualProtect(g_watchBase + view + uint64_t(first) * 4096, SIZE_T(count) * 4096, ro ? PAGE_READONLY : PAGE_READWRITE, &old) && ok;
    return ok;
}
LONG WINAPI OnWatchFault(EXCEPTION_POINTERS* e) {
    const EXCEPTION_RECORD* r = e->ExceptionRecord;
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters > 1 && r->ExceptionInformation[0] == 1 &&
        WatchFault(uintptr_t(r->ExceptionInformation[1])))
        return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

static LONG WINAPI OnException(EXCEPTION_POINTERS* e) {
    if (g_onCrash) g_onCrash(int(e->ExceptionRecord->ExceptionCode),
                             e->ExceptionRecord->NumberParameters > 1 ? uintptr_t(e->ExceptionRecord->ExceptionInformation[1]) : 0);
    TerminateProcess(GetCurrentProcess(), 139);
    return EXCEPTION_EXECUTE_HANDLER;
}
void InstallCrashHandler(CrashCallback cb) {
    g_onCrash = cb;
    AddVectoredExceptionHandler(1, OnWatchFault);
    SetUnhandledExceptionFilter(OnException);
}
void PrintBacktrace() {
    void* frames[48];
    const USHORT n = CaptureStackBackTrace(0, 48, frames, nullptr);
    const uintptr_t base = uintptr_t(GetModuleHandleW(nullptr));
    for (USHORT i = 0; i < n; i++) fprintf(stderr, "  [%u] exe+0x%llx\n", i, (unsigned long long)(uintptr_t(frames[i]) - base));
}

static std::mutex g_ioMutex;   // _lseeki64 + _read/_write : curseur partagé, sérialisé
int OpenFile(const char* path, bool write, bool create, bool truncate) {
    int flags = _O_BINARY | (write ? _O_RDWR : _O_RDONLY);
    if (create) flags |= _O_CREAT;
    if (truncate) flags |= _O_TRUNC;
    return _open(path, flags, _S_IREAD | _S_IWRITE);
}
int64_t ReadAt(int fd, void* buf, uint64_t size, uint64_t offset) {
    std::lock_guard<std::mutex> lock(g_ioMutex);
    if (_lseeki64(fd, int64_t(offset), SEEK_SET) < 0) return -1;
    return _read(fd, buf, unsigned(size));
}
int64_t WriteAt(int fd, const void* buf, uint64_t size, uint64_t offset) {
    std::lock_guard<std::mutex> lock(g_ioMutex);
    if (_lseeki64(fd, int64_t(offset), SEEK_SET) < 0) return -1;
    return _write(fd, buf, unsigned(size));
}
bool Truncate(int fd, uint64_t size) { return _chsize_s(fd, int64_t(size)) == 0; }
void CloseFile(int fd) { _close(fd); }
void RegisterCurrentThreadForDump(unsigned) {}
void DetachConsole(const std::string& journal) { if (freopen(journal.c_str(), "w", stderr)) FreeConsole(); }
void DumpAllThreadsAfter(int) {}

#else

// Diagnostic : WAKFU_PROFIL=1 autorise gdb à s'attacher au processus (Yama ptrace_scope = 1 le réserve aux ancêtres),
// pour le profilage par échantillons (runtime/tests/perf/profil.sh).
void Init() {
    if (const char* p = getenv("WAKFU_PROFIL"); p && *p == '1') prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
}
void DetachConsole(const std::string&) {}

// Mémoire invitée : 4 Go ; la mémoire physique (512 Mo, memfd) est vue en 0xA0000000 et en miroir en
// 0xC0000000 (pages de 16 Mo sur la console).
void* ReserveGuestMemory(uint64_t size) {
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    constexpr uint64_t kSize = 0x20000000ull;
    if (size < 0xE0000000ull) return p;
    const int fd = memfd_create("wakfu-phys", 0);
    bool ok = fd >= 0 && ftruncate(fd, off_t(kSize)) == 0;
    for (uint64_t view : {0xA0000000ull, 0xC0000000ull})
        ok = ok && mmap((char*)p + view, kSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) != MAP_FAILED;
    if (fd >= 0) close(fd);
    if (!ok) { munmap(p, size); return nullptr; }
    return p;
}

namespace {
bool SetPages(uint32_t first, uint32_t count, bool ro) {
    bool ok = true;
    for (uint64_t view : {0xA0000000ull, 0xC0000000ull})
        ok = mprotect(g_watchBase + view + uint64_t(first) * 4096, size_t(count) * 4096, ro ? PROT_READ : PROT_READ | PROT_WRITE) == 0 && ok;
    return ok;
}
}  // namespace

static void OnSignal(int sig, siginfo_t* info, void*) {
    if (sig == SIGSEGV && WatchFault(uintptr_t(info->si_addr))) return;   // écriture surveillée : reprise
    if (g_onCrash) g_onCrash(sig, uintptr_t(info->si_addr));
    PrintBacktrace();
    _exit(128 + sig);
}
void InstallCrashHandler(CrashCallback cb) {
    g_onCrash = cb;
    static uint8_t altStack[65536];
    stack_t ss{altStack, 0, sizeof altStack};
    sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_sigaction = OnSignal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    for (int s : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP}) sigaction(s, &sa, nullptr);
}
void PrintBacktrace() {
    void* frames[48];
    backtrace_symbols_fd(frames, backtrace(frames, 48), 2);
}

int OpenFile(const char* path, bool write, bool create, bool truncate) {
    int flags = write ? O_RDWR : O_RDONLY;
    if (create) flags |= O_CREAT;
    if (truncate) flags |= O_TRUNC;
    return open(path, flags, 0644);
}
int64_t ReadAt(int fd, void* buf, uint64_t size, uint64_t offset) { return pread(fd, buf, size, off_t(offset)); }
int64_t WriteAt(int fd, const void* buf, uint64_t size, uint64_t offset) { return pwrite(fd, buf, size, off_t(offset)); }
bool Truncate(int fd, uint64_t size) { return ftruncate(fd, off_t(size)) == 0; }
void CloseFile(int fd) { close(fd); }

namespace {
std::mutex g_dumpMutex;
std::vector<std::pair<pthread_t, unsigned>> g_dumpThreads;
thread_local unsigned t_dumpId = 0;
}
void RegisterCurrentThreadForDump(unsigned id) {
    t_dumpId = id;
    std::lock_guard<std::mutex> lock(g_dumpMutex);
    g_dumpThreads.push_back({pthread_self(), id});
}
void DumpAllThreadsAfter(int seconds) {
    signal(SIGUSR2, [](int) {
        char head[64];
        const int n = snprintf(head, sizeof head, "--- pile du thread %u ---\n", t_dumpId);
        if (write(2, head, size_t(n)) < 0) return;
        PrintBacktrace();
    });
    std::thread([seconds] {
        sleep(unsigned(seconds));
        std::vector<std::pair<pthread_t, unsigned>> ids;
        { std::lock_guard<std::mutex> lock(g_dumpMutex); ids = g_dumpThreads; }
        for (auto& [t, id] : ids) { pthread_kill(t, SIGUSR2); usleep(200000); }
    }).detach();
}

#endif

}  // namespace wk::platform
