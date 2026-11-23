#include "platform.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <algorithm>
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
#include <ctime>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace wk::platform {

namespace { CrashCallback g_onCrash = nullptr; }

#ifdef _WIN32

void Init() {
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    // Précision des minuteurs à 1 ms pour tout le processus (Windows 11 : réglage par processus, 15,6 ms sinon) :
    // Sleep, attentes à délai, KeDelayExecutionThread du jeu, horodatage KeTimeStampBundle. winmm chargé à l'exécution.
    if (HMODULE m = LoadLibraryW(L"winmm.dll"))
        if (auto f = reinterpret_cast<UINT(WINAPI*)(UINT)>(reinterpret_cast<void*>(GetProcAddress(m, "timeBeginPeriod"))))
            f(1);
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

static LONG WINAPI OnException(EXCEPTION_POINTERS* e) {
    if (g_onCrash) g_onCrash(int(e->ExceptionRecord->ExceptionCode),
                             e->ExceptionRecord->NumberParameters > 1 ? uintptr_t(e->ExceptionRecord->ExceptionInformation[1]) : 0);
    TerminateProcess(GetCurrentProcess(), 139);
    return EXCEPTION_EXECUTE_HANDLER;
}
void InstallCrashHandler(CrashCallback cb) {
    g_onCrash = cb;
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
// FreeConsole avant freopen : sinon (Wine, console créée pour le jeu lancé par WakfuRecomp) elle ferme la poignée
// standard d'erreur, devenue celle du journal, et journal.log reste vide. Console gardée si le journal est impossible.
void DetachConsole(const std::string& journal) {
    FILE* essai = fopen(journal.c_str(), "w");
    if (!essai) return;
    fclose(essai);
    FreeConsole();
    (void)!freopen(journal.c_str(), "w", stderr);
}
void ShowError(const std::string& title, const std::string& message) {
    auto large = [](const std::string& u) {
        std::wstring w(MultiByteToWideChar(CP_UTF8, 0, u.c_str(), int(u.size()), nullptr, 0), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, u.c_str(), int(u.size()), w.data(), int(w.size()));
        return w;
    };
    MessageBoxW(nullptr, large(message).c_str(), large(title).c_str(), MB_OK | MB_ICONERROR | MB_TOPMOST);
}
std::string ExeDir() {
    std::vector<wchar_t> b(32768);
    const DWORD k = GetModuleFileNameW(nullptr, b.data(), DWORD(b.size()));
    if (!k || k >= b.size()) return "";
    try { return std::filesystem::path(std::wstring(b.data(), k)).parent_path().string(); } catch (...) { return ""; }
}

int RunTool(const std::vector<std::string>& args, const char*) {
    std::string ligne;   // règles de CommandLineToArgvW : les « \ » qui précèdent un « " » (ou le guillemet fermant) sont doublés
    for (const std::string& a : args) {
        ligne += ligne.empty() ? "\"" : " \"";
        size_t barres = 0;
        for (char c : a) {
            if (c == '\\') { ++barres; continue; }
            if (c == '"') ligne.append(2 * barres + 1, '\\');
            else ligne.append(barres, '\\');
            barres = 0;
            ligne += c;
        }
        ligne.append(2 * barres, '\\');
        ligne += '"';
    }
    // mode d'erreur hérité par l'outil : un plantage de XenosRecomp/DXC ne doit pas ouvrir de fenêtre (jouer.cmd ne le règle pas) ;
    // le jeu garde son filtre OnException, seule la boîte de dialogue système disparaît
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    STARTUPINFOA si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, ligne.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                        nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = DWORD(-1);
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return int(rc);
}
void DumpAllThreadsAfter(int) {}
double ThreadCpuMs() {
    FILETIME c, e, k, u;   // unités de 100 ns
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
    const auto v = [](FILETIME f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return (v(k) + v(u)) / 1e4;
}

#else

// Diagnostic : WAKFU_PROFIL=1 autorise gdb à s'attacher au processus (Yama ptrace_scope = 1 le réserve aux ancêtres),
// pour le profilage par échantillons (runtime/tests/perf/profil.sh).
void Init() {
    if (const char* p = getenv("WAKFU_PROFIL"); p && *p == '1') prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
}
// Lancé par WakfuRecomp (WAKFU_JOURNAL=1) : stderr vers le journal, comme sous Windows ; sinon sorties inchangées
void DetachConsole(const std::string& journal) {
    if (getenv("WAKFU_JOURNAL")) (void)!freopen(journal.c_str(), "w", stderr);
}

void ShowError(const std::string&, const std::string&) {}   // stderr et journal suffisent (lanceur)

extern "C" char** environ;
std::string ExeDir() {
    std::error_code ec;
    const std::filesystem::path p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? "" : p.parent_path().string();
}

int RunTool(const std::vector<std::string>& args, const char* libDir) {
    std::vector<std::string> env;
    for (char** v = environ; *v; v++)
        if (!libDir || strncmp(*v, "LD_LIBRARY_PATH=", 16) != 0) env.push_back(*v);
    if (libDir) env.push_back(std::string("LD_LIBRARY_PATH=") + libDir);
    std::vector<char*> a, e;
    for (const std::string& s : args) a.push_back(const_cast<char*>(s.c_str()));
    for (std::string& s : env) e.push_back(s.data());
    a.push_back(nullptr);
    e.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    const int err = posix_spawn(&pid, a[0], &fa, nullptr, a.data(), e.data());
    posix_spawn_file_actions_destroy(&fa);
    int st = 0;
    if (err != 0 || waitpid(pid, &st, 0) != pid) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

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

static void OnSignal(int sig, siginfo_t* info, void*) {
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

double ThreadCpuMs() {
    timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

#endif

}  // namespace wk::platform

namespace wk::platform {
void SleepUntil(std::chrono::steady_clock::time_point t)
{
#ifdef _WIN32
    // Un minuteur par fil : CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Windows 10 1803+) ; sinon timeBeginPeriod(1)
    // (winmm chargé à l'exécution, sans dépendance de lien) et repli sur sleep_until.
    static thread_local HANDLE timer = [] {
        HANDLE h = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                                          TIMER_ALL_ACCESS);
        if (!h)
            if (HMODULE m = LoadLibraryW(L"winmm.dll"))
                if (auto f = reinterpret_cast<UINT(WINAPI*)(UINT)>(reinterpret_cast<void*>(GetProcAddress(m, "timeBeginPeriod"))))
                    f(1);
        return h;
    }();
    auto reste = t - std::chrono::steady_clock::now();
    if (reste <= reste.zero()) return;
    if (timer)
    {
        LARGE_INTEGER due;   // négatif = relatif, en unités de 100 ns
        due.QuadPart = -std::max<long long>(1, std::chrono::duration_cast<std::chrono::nanoseconds>(reste).count() / 100);
        if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0) && WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0)
            return;
    }
#endif
    std::this_thread::sleep_until(t);
}
}  // namespace wk::platform
