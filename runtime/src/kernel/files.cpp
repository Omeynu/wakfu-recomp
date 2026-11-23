// Système de fichiers invité -> hôte.
//   game:\, d:\, \Device\Cdrom0\  -> jeu de base ; update:\ -> fichiers de la TU (lecture seule)
//   autres racines (cache:, save:, …) -> <données>/<racine>/, inscriptible
// Résolution insensible à la casse ; liens symboliques ObCreateSymbolicLink appliqués avant.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "../platform.h"
#include "../process.h"
#include "stub.h"
#include "sync.h"

namespace wk {
extern std::string g_gameDir, g_overlayDir, g_dataDir;
}
using namespace wk;

namespace {
constexpr uint32_t STATUS_END_OF_FILE = 0xC0000011, STATUS_NO_MORE_FILES = 0x80000006,
                   STATUS_INVALID_HANDLE = 0xC0000008, STATUS_ACCESS_DENIED = 0xC0000022,
                   STATUS_OBJECT_NAME_COLLISION = 0xC0000035,
                   STATUS_INVALID_INFO_CLASS = 0xC0000003;

struct File {
    int fd = -1;
    bool dir = false;
    std::string guestPath, hostPath;
    std::vector<std::string> entries;   // énumération de dossier
    size_t dirPos = 0;
    uint64_t pos = 0;
};

std::mutex g_fsMutex;
std::map<std::string, std::string> g_links;   // nom de lien (minuscules, sans '\' final) -> cible

namespace fs = std::filesystem;
struct Info { bool exists = false, dir = false; uint64_t size = 0, filetime = 0; };
Info GetInfo(const std::string& p) {
    Info i;
    std::error_code ec;
    const auto st = fs::status(p, ec);
    if (ec || !fs::exists(st)) return i;
    i.exists = true;
    i.dir = fs::is_directory(st);
    if (!i.dir) i.size = fs::file_size(p, ec);
    const auto t = fs::last_write_time(p, ec);
    if (!ec) {
        const auto sys = std::chrono::file_clock::to_sys(t);
        i.filetime = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(sys.time_since_epoch()).count() / 100) +
                     116444736000000000ull;
    }
    return i;
}
std::vector<std::string> ListDir(const std::string& p) {
    std::vector<std::string> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(p, ec)) out.push_back(e.path().filename().string());
    return out;
}

std::string Lower(std::string s) { for (auto& c : s) c = char(tolower((unsigned char)c)); return s; }

uint32_t StackArg32(PPCContext& ctx, int n) { return Ld32(ctx.r1.u32 + 0x54 + (n - 8) * 8); }

// Nom ANSI_STRING invité -> chaîne
std::string AnsiString(uint32_t s) { return s ? std::string(reinterpret_cast<char*>(Host(Ld32(s + 4))), Ld16(s)) : ""; }

// Résout un composant sans tenir compte de la casse ; "" si absent.
std::string FindCi(const std::string& dir, const std::string& name) {
    const std::string exact = dir + "/" + name;
    if (GetInfo(exact).exists) return exact;
    const std::string want = Lower(name);
    for (auto& n : ListDir(dir))
        if (Lower(n) == want) return dir + "/" + n;
    return "";
}

// Chemin relatif ('\'-séparé) sous root ; createLast : le dernier composant peut ne pas exister.
std::string ResolveUnder(const std::string& root, const std::string& rel, bool createLast) {
    std::string cur = root;
    size_t i = 0;
    while (i <= rel.size()) {
        size_t j = rel.find('\\', i);
        if (j == std::string::npos) j = rel.size();
        std::string part = rel.substr(i, j - i);
        i = j + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") return "";                     // pas d'évasion du dossier racine
        std::string next = FindCi(cur, part);
        if (next.empty()) {
            if (createLast && i > rel.size()) return cur + "/" + part;
            return "";
        }
        cur = next;
    }
    return cur;
}

struct Target { std::string host; bool readOnly; };

// Chemin invité -> chemin hôte. Retourne host vide si introuvable.
Target Resolve(std::string path, bool create) {
    // liens symboliques (le plus long préfixe)
    {
        std::lock_guard<std::mutex> lock(g_fsMutex);
        std::string low = Lower(path);
        for (int pass = 0; pass < 4; pass++) {
            bool changed = false;
            for (auto& [link, target] : g_links)
                if (low.compare(0, link.size(), link) == 0 && (low.size() == link.size() || low[link.size()] == '\\')) {
                    path = target + path.substr(link.size());
                    low = Lower(path);
                    changed = true;
                    break;
                }
            if (!changed) break;
        }
    }
    std::string low = Lower(path), rel, root;
    size_t colon = path.find(':');
    static const char* kGamePrefixes[] = {"\\device\\cdrom0", "\\device\\harddisk0\\partition1\\game"};
    for (const char* p : kGamePrefixes) {
        size_t n = strlen(p);
        if (low.compare(0, n, p) == 0) { root = "game"; rel = path.substr(n); break; }
    }
    if (root.empty() && colon != std::string::npos) {
        root = low.substr(0, colon);
        if (!root.empty() && root[0] == '\\') root = root.substr(root.find_last_of('\\') + 1);   // \??\save:
        rel = path.substr(colon + 1);
    }
    if (root.empty()) { root = "game"; rel = path; }
    if (root == "game" || root == "d") return {ResolveUnder(g_gameDir, rel, false), true};
    if (root == "update") return {g_overlayDir.empty() ? "" : ResolveUnder(g_overlayDir, rel, false), true};
    std::string base = g_dataDir + "/" + root;
    std::error_code ec;
    fs::create_directories(base, ec);
    return {ResolveUnder(base, rel, create), false};
}

File* GetFile(uint32_t h) {
    HandleEntry e;
    return LookupHandle(h, &e) && e.kind == HandleKind::File ? static_cast<File*>(e.host) : nullptr;
}

void SetIo(uint32_t iosb, uint32_t status, uint32_t info) {
    if (iosb) { St32(iosb, status); St32(iosb + 4, info); }
}

// FILE_NETWORK_OPEN_INFORMATION (56 octets)
void WriteNetworkOpenInfo(uint32_t out, const Info& st) {
    St64(out + 0, st.filetime); St64(out + 8, st.filetime); St64(out + 16, st.filetime); St64(out + 24, st.filetime);
    St64(out + 32, (st.size + 0xFFF) & ~0xFFFull);
    St64(out + 40, st.size);
    St32(out + 48, st.dir ? 0x10 : 0x80);
    St32(out + 52, 0);
}

uint32_t OpenCommon(PPCContext& ctx, uint32_t handleOut, uint32_t access, uint32_t attrs, uint32_t iosb,
                    uint32_t disposition, uint32_t options) {
    const std::string path = AnsiString(Ld32(attrs + 4));
    // Diagnostic (WAKFU_SKIP_VIDEOS=1) : vidéos .wmv introuvables, pour avancer sans décodeur vidéo
    static const bool skipVideos = getenv("WAKFU_SKIP_VIDEOS") && *getenv("WAKFU_SKIP_VIDEOS") == '1';
    if (skipVideos && path.size() > 4 && Lower(path.substr(path.size() - 4)) == ".wmv") {
        fprintf(stderr, "[fichier] vidéo ignorée (WAKFU_SKIP_VIDEOS) : %s\n", path.c_str());
        SetIo(iosb, STATUS_OBJECT_NAME_NOT_FOUND, 0);
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    const bool wantsWrite = access & 0x40000006;   // GENERIC_WRITE | FILE_WRITE_DATA | FILE_APPEND_DATA
    const bool create = disposition != 1;           // tout sauf FILE_OPEN
    Target t = Resolve(path, create);
    const Info st = t.host.empty() ? Info{} : GetInfo(t.host);
    const bool exists = st.exists;
    uint32_t status = STATUS_SUCCESS, info = 1;   // FILE_OPENED
    auto fail = [&](uint32_t s) {
        fprintf(stderr, "[fichier] échec %08X : %s\n", s, path.c_str());
        SetIo(iosb, s, 0);
        return s;
    };
    if (t.host.empty()) return fail(STATUS_OBJECT_NAME_NOT_FOUND);
    if (!exists && (disposition == 1 || disposition == 4)) return fail(STATUS_OBJECT_NAME_NOT_FOUND);
    if (exists && disposition == 2) return fail(STATUS_OBJECT_NAME_COLLISION);
    if ((wantsWrite || !exists) && t.readOnly) return fail(STATUS_ACCESS_DENIED);
    auto* f = new File();
    f->guestPath = path;
    f->hostPath = t.host;
    if ((exists && st.dir) || (!exists && (options & 1))) {   // FILE_DIRECTORY_FILE
        if (!exists) { std::error_code ec; fs::create_directory(t.host, ec); info = 2; }
        f->dir = true;
        f->entries = ListDir(t.host);
        std::sort(f->entries.begin(), f->entries.end());
    } else {
        const bool truncate = exists && (disposition == 0 || disposition == 4 || disposition == 5);
        if (!exists) info = 2;
        if (truncate) info = 3;
        f->fd = platform::OpenFile(t.host.c_str(), !t.readOnly, !exists, truncate);
        if (f->fd < 0) { delete f; return fail(STATUS_ACCESS_DENIED); }
    }
    St32(handleOut, NewHandle(HandleKind::File, 0, f));
    SetIo(iosb, status, info);
    fprintf(stderr, "[fichier] ouvert %s -> %s\n", path.c_str(), t.host.c_str());
    (void)ctx;
    return status;
}
}  // namespace

namespace wk {
std::string g_overlayDir, g_dataDir = "userdata";
// Liens symboliques (montage des contenus XAM : « save: » -> « content:\<nom> »)
void AddSymbolicLink(std::string link, const std::string& target) {
    link = Lower(link);
    while (!link.empty() && (link.back() == '\\' || link.back() == ':')) link.pop_back();
    link += ":";
    std::lock_guard<std::mutex> lock(g_fsMutex);
    g_links[link] = target;
}
void RemoveSymbolicLink(std::string link) {
    link = Lower(link);
    while (!link.empty() && (link.back() == '\\' || link.back() == ':')) link.pop_back();
    link += ":";
    std::lock_guard<std::mutex> lock(g_fsMutex);
    g_links.erase(link);
}
// Dossier hôte d'un contenu XAM (créé si demandé) ; "" si absent
std::string ContentDir(const std::string& name, bool create) {
    std::string clean;
    for (char c : name) clean += (isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.') ? c : '_';
    const std::string dir = g_dataDir + "/content/" + clean;
    std::error_code ec;
    if (create) fs::create_directories(dir, ec);
    return fs::is_directory(dir, ec) ? dir : "";
}
void CloseFileHandle(void* host) {
    auto* f = static_cast<File*>(host);
    if (f->fd >= 0) platform::CloseFile(f->fd);
    delete f;
}
}  // namespace wk

WK_IMPORT(NtCreateFile) {
    WK_TRACE(NtCreateFile);
    // (handle*, accès, attributs, iosb, taille_alloc*, attributs_fichier, partage, disposition, options)
    Ret(ctx, OpenCommon(ctx, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r10.u32, StackArg32(ctx, 8)));
}
WK_IMPORT(NtOpenFile) {
    WK_TRACE(NtOpenFile);
    // (handle*, accès, attributs, iosb, partage, options)
    Ret(ctx, OpenCommon(ctx, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, 1, ctx.r8.u32));
}
WK_IMPORT(NtClose) {
    WK_TRACE(NtClose);
    HandleEntry e;
    if (!CloseHandle(ctx.r3.u32, &e)) { Ret(ctx, STATUS_INVALID_HANDLE); return; }
    if (e.kind == HandleKind::File) CloseFileHandle(e.host);
    Ret(ctx, STATUS_SUCCESS);
}
static void Complete(uint32_t event, uint32_t iosb, uint32_t status, uint32_t info) {
    SetIo(iosb, status, info);
    if (event) {
        HandleEntry e;
        if (LookupHandle(event, &e)) { std::lock_guard<std::mutex> lock(g_kernelLock); SignalLocked(e.object, 1); }
    }
}
WK_IMPORT(NtReadFile) {
    WK_TRACE(NtReadFile);
    // (handle, événement, apc, apc_ctx, iosb, tampon, longueur, LARGE_INTEGER* position)
    File* f = GetFile(ctx.r3.u32);
    if (!f || f->dir) { Ret(ctx, STATUS_INVALID_HANDLE); return; }
    uint64_t off = f->pos;
    if (ctx.r10.u32) { int64_t o = int64_t(Ld64(ctx.r10.u32)); if (o >= 0) off = uint64_t(o); }
    const uint32_t dst = ctx.r8.u32;
    int64_t n = platform::ReadAt(f->fd, Host(dst), ctx.r9.u32, off);
    if (n < 0) n = 0;
    f->pos = off + n;
    const uint32_t status = (n == 0 && ctx.r9.u32) ? STATUS_END_OF_FILE : STATUS_SUCCESS;
    Complete(ctx.r4.u32, ctx.r7.u32, status, uint32_t(n));
    Ret(ctx, status);
}
WK_IMPORT(NtWriteFile) {
    WK_TRACE(NtWriteFile);
    File* f = GetFile(ctx.r3.u32);
    if (!f || f->dir) { Ret(ctx, STATUS_INVALID_HANDLE); return; }
    uint64_t off = f->pos;
    if (ctx.r10.u32) { int64_t o = int64_t(Ld64(ctx.r10.u32)); if (o >= 0) off = uint64_t(o); }
    int64_t n = platform::WriteAt(f->fd, Host(ctx.r8.u32), ctx.r9.u32, off);
    if (n < 0) { Complete(ctx.r4.u32, ctx.r7.u32, STATUS_ACCESS_DENIED, 0); Ret(ctx, STATUS_ACCESS_DENIED); return; }
    f->pos = off + n;
    Complete(ctx.r4.u32, ctx.r7.u32, STATUS_SUCCESS, uint32_t(n));
    Ret(ctx, STATUS_SUCCESS);
}
WK_IMPORT(NtFlushBuffersFile) { WK_TRACE(NtFlushBuffersFile); Ret(ctx, STATUS_SUCCESS); }
WK_IMPORT(NtQueryInformationFile) {
    WK_TRACE(NtQueryInformationFile);
    // (handle, iosb, info, longueur, classe)
    File* f = GetFile(ctx.r3.u32);
    if (!f) { Ret(ctx, STATUS_INVALID_HANDLE); return; }
    const Info st = GetInfo(f->hostPath);
    const uint32_t out = ctx.r5.u32, cls = ctx.r7.u32;
    uint32_t len = 0;
    switch (cls) {
        case 5:   // FileStandardInformation
            St64(out, (st.size + 0xFFF) & ~0xFFFull); St64(out + 8, st.size);
            St32(out + 16, 1); St8(out + 20, 0); St8(out + 21, f->dir ? 1 : 0); len = 24; break;
        case 14:  // FilePositionInformation
            St64(out, f->pos); len = 8; break;
        case 34:  // FileNetworkOpenInformation
            WriteNetworkOpenInfo(out, st); len = 56; break;
        case 20:  // FileEndOfFileInformation
            St64(out, st.size); len = 8; break;
        default:
            fprintf(stderr, "[fichier] NtQueryInformationFile classe %u non gérée\n", cls);
            SetIo(ctx.r4.u32, STATUS_INVALID_INFO_CLASS, 0);
            Ret(ctx, STATUS_INVALID_INFO_CLASS);
            return;
    }
    SetIo(ctx.r4.u32, STATUS_SUCCESS, len);
    Ret(ctx, STATUS_SUCCESS);
}
WK_IMPORT(NtSetInformationFile) {
    WK_TRACE(NtSetInformationFile);
    File* f = GetFile(ctx.r3.u32);
    if (!f) { Ret(ctx, STATUS_INVALID_HANDLE); return; }
    const uint32_t in = ctx.r5.u32, cls = ctx.r7.u32;
    uint32_t status = STATUS_SUCCESS;
    switch (cls) {
        case 14: f->pos = Ld64(in); break;                                          // position
        case 20: case 19:                                                            // fin / allocation
            if (f->fd >= 0 && !platform::Truncate(f->fd, Ld64(in))) status = STATUS_ACCESS_DENIED; break;
        case 13:                                                                     // suppression
            if (Ld8(in)) { std::error_code ec; if (!fs::remove(f->hostPath, ec)) status = STATUS_ACCESS_DENIED; } break;
        default:
            fprintf(stderr, "[fichier] NtSetInformationFile classe %u non gérée\n", cls);
            status = STATUS_INVALID_INFO_CLASS;
    }
    SetIo(ctx.r4.u32, status, 0);
    Ret(ctx, status);
}
WK_IMPORT(NtQueryFullAttributesFile) {
    WK_TRACE(NtQueryFullAttributesFile);
    const std::string path = AnsiString(Ld32(ctx.r3.u32 + 4));
    Target t = Resolve(path, false);
    const Info st = t.host.empty() ? Info{} : GetInfo(t.host);
    if (!st.exists) { Ret(ctx, STATUS_OBJECT_NAME_NOT_FOUND); return; }
    WriteNetworkOpenInfo(ctx.r4.u32, st);
    Ret(ctx, STATUS_SUCCESS);
}
WK_IMPORT(NtQueryVolumeInformationFile) {
    WK_TRACE(NtQueryVolumeInformationFile);
    const uint32_t out = ctx.r5.u32, cls = ctx.r7.u32;
    uint32_t len = 0;
    switch (cls) {
        case 1:   // FileFsVolumeInformation
            St64(out, 0); St32(out + 8, 0x12345678); St32(out + 12, 0); St8(out + 16, 0); len = 20; break;
        case 3:   // FileFsSizeInformation : 4 Go libres, unités de 16 Kio
            St64(out, 0x40000); St64(out + 8, 0x40000); St32(out + 16, 32); St32(out + 20, 512); len = 24; break;
        case 5: { // FileFsAttributeInformation : FATX
            St32(out, 0); St32(out + 4, 255); St32(out + 8, 8);
            const char* n = "FATX";
            for (int i = 0; i < 4; i++) St16(out + 12 + 2 * i, uint16_t(n[i]));
            len = 20; break;
        }
        default:
            fprintf(stderr, "[fichier] NtQueryVolumeInformationFile classe %u non gérée\n", cls);
            SetIo(ctx.r4.u32, STATUS_INVALID_INFO_CLASS, 0);
            Ret(ctx, STATUS_INVALID_INFO_CLASS);
            return;
    }
    SetIo(ctx.r4.u32, STATUS_SUCCESS, len);
    Ret(ctx, STATUS_SUCCESS);
}
WK_IMPORT(NtQueryDirectoryFile) {
    WK_TRACE(NtQueryDirectoryFile);
    // (handle, événement, apc, apc_ctx, iosb, info, longueur, ANSI_STRING* filtre, recommencer)
    File* f = GetFile(ctx.r3.u32);
    if (!f || !f->dir) { Ret(ctx, STATUS_INVALID_HANDLE); return; }
    if (StackArg32(ctx, 8)) f->dirPos = 0;
    std::string filter = Lower(AnsiString(ctx.r10.u32));
    auto match = [&](const std::string& n) {
        if (filter.empty() || filter == "*" || filter == "*.*") return true;
        if (filter[0] == '*') { std::string ext = filter.substr(1), l = Lower(n); return l.size() >= ext.size() && l.compare(l.size() - ext.size(), ext.size(), ext) == 0; }
        return Lower(n) == filter;
    };
    while (f->dirPos < f->entries.size() && !match(f->entries[f->dirPos])) f->dirPos++;
    if (f->dirPos >= f->entries.size()) {
        Complete(ctx.r4.u32, ctx.r7.u32, STATUS_NO_MORE_FILES, 0);
        Ret(ctx, STATUS_NO_MORE_FILES);
        return;
    }
    const std::string& name = f->entries[f->dirPos++];
    const Info st = GetInfo(f->hostPath + "/" + name);
    // X_FILE_DIRECTORY_INFORMATION : suivant, index, 4 dates, fin, allocation, attributs, longueur du nom, nom
    const uint32_t out = ctx.r8.u32, need = 0x40 + uint32_t(name.size());
    if (ctx.r9.u32 < need) { Ret(ctx, 0x80000005); return; }   // BUFFER_OVERFLOW
    St32(out, 0); St32(out + 4, 0);
    WriteNetworkOpenInfo(out + 8, st);     // dates (+8..+0x27) et attributs (+0x38) aux mêmes positions
    St64(out + 0x28, st.size);                          // ici : fin de fichier d'abord,
    St64(out + 0x30, (st.size + 0xFFF) & ~0xFFFull);    // puis taille allouée
    St32(out + 0x3C, uint32_t(name.size()));
    memcpy(Host(out + 0x40), name.data(), name.size());
    Complete(ctx.r4.u32, ctx.r7.u32, STATUS_SUCCESS, need);
    Ret(ctx, STATUS_SUCCESS);
}
WK_IMPORT(ObCreateSymbolicLink) {
    WK_TRACE(ObCreateSymbolicLink);
    std::string link = Lower(AnsiString(ctx.r3.u32)), target = AnsiString(ctx.r4.u32);
    while (!link.empty() && link.back() == '\\') link.pop_back();
    if (link.compare(0, 4, "\\??\\") == 0) link = link.substr(4);
    fprintf(stderr, "[fichier] lien %s -> %s\n", link.c_str(), target.c_str());
    std::lock_guard<std::mutex> lock(g_fsMutex);
    g_links[link] = target;
    Ret(ctx, STATUS_SUCCESS);
}
WK_IMPORT(ObDeleteSymbolicLink) {
    WK_TRACE(ObDeleteSymbolicLink);
    std::string link = Lower(AnsiString(ctx.r3.u32));
    if (link.compare(0, 4, "\\??\\") == 0) link = link.substr(4);
    std::lock_guard<std::mutex> lock(g_fsMutex);
    g_links.erase(link);
    Ret(ctx, STATUS_SUCCESS);
}
