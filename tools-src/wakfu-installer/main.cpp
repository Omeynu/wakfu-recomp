// WakfuRecomp (Windows : WakfuRecomp.exe ; Linux : WakfuRecomp) : lanceur et installateur en un seul fichier.
// 1. extrait l'archive ajoutée à la fin de l'exécutable dans <dossier>/outils-<id> (une fois par version).
//    Windows : outils prêts (llvm-mingw, CMake, ninja, XenonRecomp, wakfu-prepare, XenosRecomp + DXC, bibliothèques) ;
//    Linux : sources (runtime, wakfu-prepare, XenonRecomp, XenosRecomp) + DXC, outils compilés ici (clang, CMake, ninja
//    du système) ;
// 2. prépare le jeu de l'utilisateur (wakfu-prepare : STFS / ISO-XISO / dossier + TU) ;
// 3. génère le C++ (XenonRecomp patché) et les stubs d'imports ; 4. compile wakfu.exe / wakfu (CMake + ninja + Clang) ;
// 5. shaders du jeu en SPIR-V (XenosRecomp + DXC) pour le rendu natif.
// Sans argument : fenêtre du lanceur (lanceur.cpp) ; avec des arguments : ligne de commande ci-dessous (tests).
#ifdef _WIN32
#define SDL_MAIN_HANDLED
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#else
#include <csignal>
#include <fcntl.h>
#include <dlfcn.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <zstd.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "../../runtime/src/config.h"   // pas de -I runtime/src (cf. lanceur.cpp)
#include "../../runtime/src/png.h"
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#define XXH_INLINE_ALL
#include <xxhash.h>
#include "gen_imports.h"
#include "installer.h"

namespace wk {

Suivi* g_suivi = nullptr;

namespace {

#ifdef _WIN32
#define A(s) L##s
constexpr const char* kExe = ".exe";
constexpr const char* kSdk = "sdk";   // en-têtes et tables de XenonRecomp dans l'archive
HANDLE g_job = nullptr;   // fenêtre : outils lancés dans un « job », arrêtés si le lanceur se ferme
#else
#define A(s) s
constexpr const char* kExe = "";
constexpr const char* kSdk = "xenonrecomp";   // sources complètes (même arborescence XenonUtils, thirdparty/simde)
std::atomic<pid_t> g_enfant{ 0 };   // groupe de processus de l'outil en cours (arrêté si le lanceur se ferme)
#endif
// RECOMPLAUNCHER=1 : lancé par RecompLauncher (>= 0.9.3), sans console (SuiviRecompLauncher)
const bool g_recompLauncher = [] { const char* v = std::getenv("RECOMPLAUNCHER"); return v && std::strcmp(v, "1") == 0; }();
using Arg = fs::path::string_type;   // argument de ligne de commande (wstring sous Windows)
fs::path exe(fs::path p) { p += kExe; return p; }

void say(const char* f, ...) __attribute__((format(printf, 1, 2)));
void say(const char* f, ...)
{
    va_list a;
    va_start(a, f);
    if (g_suivi)   // fenêtre : vers le journal
    {
        char b[4096];
        std::vsnprintf(b, sizeof b, f, a);
        g_suivi->ligne(b);
    }
    else std::vprintf(f, a);
    va_end(a);
    std::fflush(stdout);
}

// ---- Archive ----
struct Trailer { uint64_t off = 0, size = 0; std::string id; };   // = champs off, size, id d'Install

Trailer readTrailer(const fs::path& self)
{
    std::ifstream f(self, std::ios::binary | std::ios::ate);
    uint64_t total = uint64_t(f.tellg());
    char t[48];
    if (!f || total < sizeof t) throw Error("lecture de l'exécutable impossible");
    f.seekg(total - sizeof t);
    f.read(t, sizeof t);
    Trailer r;
    if (!f || std::memcmp(t, "WAKFUPK1", 8) != 0) throw Error("archive absente : exécutable incomplet ou endommagé");
    std::memcpy(&r.off, t + 8, 8);
    std::memcpy(&r.size, t + 16, 8);
    r.id.assign(t + 24, 16);
    for (char c : r.id)
        if (!std::isxdigit(static_cast<unsigned char>(c))) throw Error("archive endommagée (identifiant)");
    if (r.off + r.size + sizeof t != total) throw Error("archive endommagée (taille)");
    return r;
}

class ZReader
{
public:
    ZReader(const fs::path& p, uint64_t off, uint64_t size) : in_(p, std::ios::binary), left_(size), ctx_(ZSTD_createDCtx()),
        ib_(ZSTD_DStreamInSize()), ob_(ZSTD_DStreamOutSize())
    {
        in_.seekg(off);
        if (!in_ || !ctx_) throw Error("ouverture de l'archive impossible");
        zin_ = { ib_.data(), 0, 0 };
    }
    ~ZReader() { ZSTD_freeDCtx(ctx_); }
    void read(void* dst, size_t n)
    {
        auto d = static_cast<char*>(dst);
        while (n)
        {
            if (pos_ == len_) fill();
            size_t k = std::min(n, len_ - pos_);
            std::memcpy(d, ob_.data() + pos_, k);
            pos_ += k, d += k, n -= k;
        }
    }
    uint64_t consumed() const { return consumed_; }

private:
    void fill()
    {
        for (;;)
        {
            if (zin_.pos == zin_.size)
            {
                if (!left_) throw Error("archive tronquée");
                size_t k = size_t(std::min<uint64_t>(ib_.size(), left_));
                in_.read(ib_.data(), k);
                if (!in_) throw Error("lecture de l'archive impossible");
                left_ -= k, consumed_ += k;
                zin_ = { ib_.data(), k, 0 };
            }
            ZSTD_outBuffer o = { ob_.data(), ob_.size(), 0 };
            size_t r = ZSTD_decompressStream(ctx_, &o, &zin_);
            if (ZSTD_isError(r)) throw Error(std::string("archive endommagée : ") + ZSTD_getErrorName(r));
            if (o.pos) { pos_ = 0, len_ = o.pos; return; }
        }
    }
    std::ifstream in_;
    uint64_t left_, consumed_ = 0;
    ZSTD_DCtx* ctx_;
    std::vector<char> ib_, ob_;
    ZSTD_inBuffer zin_;
    size_t pos_ = 0, len_ = 0;
};

// Chemin relatif sûr : composants non vides, ni « . » ni « .. », aucun caractère interdit sous Windows
fs::path safeRel(const std::string& s)
{
    if (s.empty() || s.size() > 400 || s[0] == '/') throw Error("archive : chemin invalide");
    fs::path r;
    size_t a = 0;
    while (a <= s.size())
    {
        size_t b = s.find('/', a);
        if (b == std::string::npos) b = s.size();
        std::string c = s.substr(a, b - a);
        if (c.empty() || c == "." || c == "..") throw Error("archive : chemin invalide : " + s);
        for (unsigned char ch : c)
            if (ch < 32 || std::strchr("\\:*?\"<>|", ch)) throw Error("archive : chemin invalide : " + s);
        r /= fromU8(c);
        a = b + 1;
    }
    return r;
}

void extractPayload(const fs::path& self, const Trailer& t, const fs::path& dst)
{
    fs::create_directories(dst);
    ZReader z(self, t.off, t.size);
    char magic[8];
    z.read(magic, 8);
    if (std::memcmp(magic, "WKPK0001", 8) != 0) throw Error("archive : format inconnu");
    std::vector<char> buf(1 << 20);
    uint64_t files = 0, next = 0;
    for (;;)
    {
        char type;
        z.read(&type, 1);
        if (type == 'E') break;
        if (type != 'D' && type != 'F') throw Error("archive : enregistrement inconnu");
        uint16_t len;
        uint64_t size = 0;
        z.read(&len, 2);
        if (type == 'F') z.read(&size, 8);
        std::string name(len, '\0');
        z.read(name.data(), len);
        fs::path p = dst / safeRel(name);
        if (type == 'D') { fs::create_directories(p); continue; }
        std::ofstream o(p, std::ios::binary | std::ios::trunc);
        if (!o) throw Error("écriture impossible : " + u8(p));
        while (size)
        {
            size_t k = size_t(std::min<uint64_t>(size, buf.size()));
            z.read(buf.data(), k);
            o.write(buf.data(), k);
            size -= k;
        }
        if (!o.flush()) throw Error("écriture impossible (disque plein ?) : " + u8(p));
        ++files;
        if (g_suivi) g_suivi->avance(double(z.consumed()) / double(t.size));
        else if (z.consumed() >= next)
        {
            say("\r  %3u %%", unsigned(z.consumed() * 100 / t.size));
            next += t.size / 50;
        }
    }
    say("\r  %llu fichiers extraits\n", (unsigned long long)files);
}


// ---- Processus ----
#ifdef _WIN32
// Règles de découpage de CommandLineToArgvW / CRT
std::wstring quote(const std::wstring& a)
{
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring r = L"\"";
    for (size_t i = 0;; ++i)
    {
        size_t bs = 0;
        while (i < a.size() && a[i] == L'\\') ++i, ++bs;
        if (i == a.size()) { r.append(bs * 2, L'\\'); break; }
        if (a[i] == L'"') r.append(bs * 2 + 1, L'\\');
        else r.append(bs, L'\\');
        r += a[i];
    }
    return r + L"\"";
}

// Ligne de commande : les outils écrivent sur la console (ou les sorties redirigées) du lanceur.
// Fenêtre et RecompLauncher : sortie lue par un tube et envoyée au suivi (journal, progression), sans fenêtre de
// console, dans le job du lanceur. jeu = wakfu.exe : ni tube ni job (fenêtre : sa propre console ; RecompLauncher :
// sans fenêtre, sorties héritées).
// sansConsole (jeu, option console = false) : aucune console créée, pas même un instant (le journal va dans journal.log).
int runProcess(const std::vector<Arg>& args, const fs::path& cwd = {}, bool jeu = false, bool sansConsole = false)
{
    std::wstring cmd;
    for (auto& a : args) cmd += (cmd.empty() ? L"" : L" ") + quote(a);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    DWORD flags = 0;
    HANDLE rd = nullptr, wr = nullptr;
    const bool capture = g_suivi && !jeu;
    if (capture)
    {
        SECURITY_ATTRIBUTES sa{ sizeof sa, nullptr, TRUE };
        if (!CreatePipe(&rd, &wr, &sa, 0)) throw Error("création d'un tube impossible");
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = si.hStdError = wr;
        flags = CREATE_NO_WINDOW | CREATE_SUSPENDED;
    }
    else if ((!g_suivi || g_recompLauncher) && !GetConsoleWindow())
    {
        // Ligne de commande sans console (sorties redirigées : tests) : l'exécutable est une application à fenêtre,
        // ses descripteurs standard sont donc passés explicitement, et les outils ne doivent pas ouvrir de console.
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        flags = CREATE_NO_WINDOW;
    }
    if (jeu && sansConsole) flags |= CREATE_NO_WINDOW;
    BOOL ok = CreateProcessW(args[0].c_str(), cmd.data(), nullptr, nullptr, TRUE, flags, nullptr,
                             cwd.empty() ? nullptr : cwd.c_str(), &si, &pi);
    DWORD err = GetLastError();
    if (wr) CloseHandle(wr);
    if (!ok)
    {
        if (rd) CloseHandle(rd);
        throw Error("lancement impossible : " + u8(args[0]) + " (erreur " + std::to_string(err) + ")");
    }
    if (capture)
    {
        if (g_job) AssignProcessToJobObject(g_job, pi.hProcess);
        ResumeThread(pi.hThread);
        // Lignes terminées par \n ou \r (progressions réécrites sur place)
        std::string cur;
        char b[4096];
        DWORD n;
        while (ReadFile(rd, b, sizeof b, &n, nullptr) && n)
            for (DWORD i = 0; i < n; ++i)
            {
                if (b[i] == '\n' || b[i] == '\r') { if (!cur.empty()) g_suivi->ligne(cur); cur.clear(); }
                else cur += b[i];
            }
        if (!cur.empty()) g_suivi->ligne(cur);
        CloseHandle(rd);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = 1;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return int(rc);
}

#else
// Ligne de commande : sorties héritées. Fenêtre : sortie lue par un tube et envoyée au suivi ; l'outil a son propre
// groupe de processus (arrêté avec ses enfants par quitterDeSuite) et meurt avec le lanceur (PR_SET_PDEATHSIG).
// jeu : ni tube ni groupe. Commande sans « / » : cherchée dans PATH (outils du système).
int runProcess(const std::vector<Arg>& args, const fs::path& cwd = {}, bool jeu = false, bool = false)
{
    const bool capture = g_suivi && !jeu;
    int fd[2] = { -1, -1 };
    if (capture && pipe2(fd, O_CLOEXEC) != 0) throw Error("création d'un tube impossible");
    std::vector<char*> v;
    for (auto& a : args) v.push_back(const_cast<char*>(a.c_str()));
    v.push_back(nullptr);
    pid_t pid = fork();
    if (pid == 0)
    {
        if (g_suivi && !jeu) { setpgid(0, 0); prctl(PR_SET_PDEATHSIG, SIGKILL); }
        if (capture) { dup2(fd[1], 1); dup2(fd[1], 2); }
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(127);
        execvp(v[0], v.data());
        _exit(127);
    }
    if (fd[1] >= 0) close(fd[1]);
    if (pid < 0)
    {
        if (fd[0] >= 0) close(fd[0]);
        throw Error("lancement impossible : " + args[0]);
    }
    if (capture)
    {
        g_enfant = pid;
        std::string cur;
        char b[4096];
        ssize_t n;
        while ((n = read(fd[0], b, sizeof b)) > 0 || (n < 0 && errno == EINTR))
            for (ssize_t i = 0; i < n; ++i)
            {
                if (b[i] == '\n' || b[i] == '\r') { if (!cur.empty()) g_suivi->ligne(cur); cur.clear(); }
                else cur += b[i];
            }
        if (!cur.empty()) g_suivi->ligne(cur);
        close(fd[0]);
    }
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    g_enfant = 0;
    if (WIFEXITED(st) && WEXITSTATUS(st) == 127) throw Error("lancement impossible : " + args[0] + " (introuvable ?)");
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}
#endif

int step(const char* what, const std::vector<Arg>& args, const fs::path& cwd = {}, int accepte = 0)
{
    int rc = runProcess(args, cwd);
    if (rc != 0 && rc != accepte) throw Error(std::string(what) + " : échec (code " + std::to_string(rc) + ")");
    return rc;
}

Arg w(const fs::path& p) { return p.native(); }
Arg wdef(const char* name, const fs::path& p) { return A("-D") + fs::path(name).native() + A("=") + fromU8(u8(p)).native(); }
void etape(Etape e) { if (g_suivi) g_suivi->etape(e); }

// Un cache CMake fait depuis un autre emplacement (dossier de travail déplacé) bloque cmake : build refait
void propre(const fs::path& D, const fs::path& source, const fs::path& build)
{
    std::ifstream f(build / "CMakeCache.txt");
    std::string l, attendu = "CMAKE_HOME_DIRECTORY:INTERNAL=" + u8(source);
    while (std::getline(f, l))
        if (l == attendu) return;
    supprimerDans(D, build);
}

// ---- Console ----
#ifdef _WIN32
bool g_ownConsole = false;   // console créée pour nous (glisser-déposer sur l'exécutable) : elle se ferme à la sortie

// L'exécutable est une application à fenêtre : en ligne de commande, il écrit dans les sorties redirigées s'il y en
// a, sinon dans la console de la fenêtre qui l'a lancé, sinon dans une console à lui.
void openConsole()
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN) return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
    {
        AllocConsole();
        g_ownConsole = true;
    }
    std::freopen("CONOUT$", "w", stdout);
    std::freopen("CONOUT$", "w", stderr);
    std::freopen("CONIN$", "r", stdin);
}

std::wstring readLine()
{
    wchar_t b[4096];
    DWORD n = 0;
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    if (ReadConsoleW(h, b, 4095, &n, nullptr)) b[n] = 0;
    else if (!fgetws(b, 4096, stdin)) return {};
    std::wstring s(b);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
    return s;
}

std::vector<fs::path> splitPaths(const std::wstring& line)   // chemins collés ou glissés (guillemets si espaces)
{
    std::vector<fs::path> r;
    int n = 0;
    std::wstring l = L"x " + line;
    LPWSTR* v = CommandLineToArgvW(l.c_str(), &n);
    for (int i = 1; v && i < n; ++i) r.emplace_back(v[i]);
    LocalFree(v);
    return r;
}

fs::path envPath(const wchar_t* name)
{
    wchar_t b[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(name, b, MAX_PATH * 2);
    return n && n < MAX_PATH * 2 ? fs::path(b) : fs::path();
}

#else
fs::path envPath(const char* name)
{
    const char* v = std::getenv(name);
    return v && *v ? fs::path(v) : fs::path();
}
#endif

bool isInside(const fs::path& p, const fs::path& dir)
{
    auto r = fs::weakly_canonical(p).lexically_relative(fs::weakly_canonical(dir));
    return !r.empty() && *r.begin() != "..";
}

// Ligne de commande : entrées refusées avant toute extraction, avec la cause (mêmes contrôles que la fenêtre)
void verifierEntrees(const std::vector<fs::path>& inputs)
{
    bool jeu = false, maj = false, dossier = false;
    for (auto& i : inputs)
    {
        InfoPaquet info;
        Paquet r = verifierPaquet(i, &info);
        char tid[16];
        std::snprintf(tid, sizeof tid, "%08X", info.titre);
        std::string cause;
        switch (r)
        {
        case Paquet::Jeu: jeu = true; continue;
        case Paquet::MiseAJour: maj = true; continue;
        case Paquet::Dossier: dossier = true; continue;   // dossier quelconque : examiné par wakfu-prepare
        case Paquet::NonStfs: cause = "ni paquet Xbox 360 (STFS), ni image ISO/XISO, ni dossier extrait"; break;
        case Paquet::AutreJeu: cause = "paquet d'un autre jeu (« " + info.nom + " », titre " + tid + ")"; break;
        case Paquet::NiJeuNiMaj: cause = "ce paquet ne contient ni le jeu ni sa mise à jour"; break;
        case Paquet::Tronque: cause = "fichier tronqué (copie incomplète)"; break;
        case Paquet::MajNonPriseEnCharge: cause = "version de mise à jour non prise en charge (« " + info.nom + " ») : Title Update 2 attendue"; break;
        case Paquet::DossierMixte: cause = "ce dossier contient default.xex et default.xexp : séparer le jeu et la mise à jour"; break;
        case Paquet::Illisible: cause = "fichier introuvable, illisible ou endommagé"; break;
        }
        throw Error(u8(i) + " : " + cause);
    }
    if (dossier) return;
    if (!jeu) throw Error(maj ? "jeu absent : seule la mise à jour a été donnée (jeu et mise à jour inversés ?)" : "jeu absent");
    if (!maj) throw Error("mise à jour (Title Update 2) absente : donner aussi le fichier TU_...");
}

int usage()
{
    say("WakfuRecomp : construit Islands of Wakfu à partir de VOTRE copie du jeu (Xbox 360).\n\n"
        "usage : WakfuRecomp [--dossier <D>] [--sans-lancer] <jeu> [<mise à jour>]\n"
        "  <jeu>, <mise à jour> : paquet XBLA (STFS) du jeu et de la Title Update 2, image ISO/XISO, dossier extrait,\n"
        "                         ou un dossier qui contient ces fichiers. Glisser-déposer sur l'exécutable fonctionne.\n"
        "  --dossier <D>  : dossier de travail (défaut %%LOCALAPPDATA%%\\WakfuRecomp, Linux ~/.local/share/wakfu-recomp ;\n"
        "                   prévoir ~3 Go libres)\n"
        "  --sans-lancer  : ne pas lancer le jeu à la fin\n"
        "Rendu natif Vulkan (shaders du jeu compilés en SPIR-V à l'installation).\n"
        "Sans argument : fenêtre du lanceur (choix du jeu, recompilation, réglages, jeu, désinstallation).\n");
    return 2;
}

} // namespace

fs::path selfPath()
{
#ifdef _WIN32
    wchar_t selfBuf[32768];
    DWORD n = GetModuleFileNameW(nullptr, selfBuf, 32768);
    if (!n || n >= 32768) throw Error("chemin de l'exécutable introuvable");
    return fs::path(selfBuf);
#else
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    if (ec) throw Error("chemin de l'exécutable introuvable");
    return p;
#endif
}

fs::path appDataDir()
{
#ifdef _WIN32
    fs::path a = envPath(L"APPDATA");
    return a.empty() ? a : a / "WakfuRecomp";
#else
    return {};   // Linux : données toujours dans <D>/userdata
#endif
}

#ifndef _WIN32
fs::path dataHome()   // ~/.local/share (ou $XDG_DATA_HOME)
{
    if (fs::path x = envPath("XDG_DATA_HOME"); x.is_absolute()) return x;
    fs::path h = envPath("HOME");
    if (h.empty()) throw Error("HOME non défini : utiliser --dossier <D>");
    return h / ".local" / "share";
}
#endif

fs::path defaultWorkDir()
{
#ifdef _WIN32
    fs::path la = envPath(L"LOCALAPPDATA");
    if (la.empty()) throw Error("LOCALAPPDATA non défini : utiliser --dossier <D>");
    return la / "WakfuRecomp";
#else
    return dataHome() / "wakfu-recomp";
#endif
}

void openWorkDir(Install& in, fs::path D)
{
    fs::create_directories(D);
    D = fs::weakly_canonical(fs::absolute(D));   // chemin réel : la garde de suppression raisonne dessus
    // Le dossier de travail est vidé par endroits (config, work, outils-*) : refuser un dossier non vide qui
    // n'a pas été créé par l'installateur (marqueur), pour ne jamais effacer de fichiers de l'utilisateur.
    if (!fs::exists(D / ".wakfurecomp"))
    {
        if (!fs::is_empty(D))
            throw Error("le dossier " + u8(D) + " n'est pas vide et n'a pas été créé par WakfuRecomp : choisir un dossier vide avec --dossier");
        std::ofstream(D / ".wakfurecomp").close();
    }
    in.D = D;
    in.W = D / "work";
    in.G = in.W / "game";
    Trailer t = readTrailer(selfPath());
    in.off = t.off, in.size = t.size, in.id = t.id;
    in.T = D / ("outils-" + t.id);
    in.bin = in.T / "bin";
}

// Données du joueur (sauvegardes, réglages, config.toml) : %APPDATA%\WakfuRecomp (itinérant), sinon <D>\userdata.
// Première fois : l'ancien <D>\userdata est copié (jamais supprimé), pour ne perdre aucune sauvegarde.
// Ancien mode portable (retiré) : sauvegardes de <exe>\WakfuRecomp (Linux <exe>/wakfu-recomp) copiées une fois.
// Seul point de calcul du dossier des données (ligne de commande, fenêtre, jouer.cmd).

fs::path dataDir(const fs::path& D)
{
    fs::path a = appDataDir().parent_path();
    fs::path target = a.empty() ? D / "userdata" : a / "WakfuRecomp";
    std::error_code ec;
    if (!a.empty() && !fs::exists(target) && fs::is_directory(D / "userdata") && !fs::is_empty(D / "userdata", ec))
    {
        fs::copy(D / "userdata", target, fs::copy_options::recursive, ec);
        if (ec) throw Error("copie de " + u8(D / "userdata") + " vers " + u8(target) + " : " + ec.message());
        say("Données copiées vers %s (l'ancien dossier est conservé)\n", u8(target).c_str());
    }
    fs::create_directories(target);
    const fs::path ancien = selfPath().parent_path() / (kExe[0] ? "WakfuRecomp" : "wakfu-recomp");
    if (int64_t n = migrerPortable(ancien, target); n >= 0)
        say("Ancien mode portable : %lld fichier(s) copié(s) de %s vers %s (fichiers existants gardés, ancien dossier conservé)\n",
            (long long)n, u8(ancien / "userdata").c_str(), u8(target).c_str());
    return target;
}

bool isPrepared(const Install& in) { return fs::exists(in.G / "patched" / "image.bin"); }
bool isInstalled(const Install& in) { return isPrepared(in) && fs::exists(in.D / exe("wakfu")); }
bool isUpToDate(const Install& in)
{
    std::string id;
    std::ifstream(in.W / "out" / ".recomp-ok") >> id;
    return id == in.id;
}
double freeGb(const fs::path& D)
{
    std::error_code ec;
    auto i = fs::space(D, ec);
    return ec ? -1 : i.available / double(1 << 30);
}
void forceRebuild(const Install& in)
{
    fs::remove(in.W / "out" / ".recomp-ok");
    supprimerDans(in.D, in.W / "build");
}

// Shaders du jeu en SPIR-V (XenosRecomp + DXC officiel, outils-<id>/natif), comme
// packaging/shaders/precompile-shaders.sh : <D>/spirv/<XXH3 du conteneur>.spv. Sources : <D>/spirv/conteneurs
// (Media/Shaders, conteneurs du XEX, puis ceux que le jeu dépose à l'exécution : traduits au lancement suivant).
// Échecs notés dans spirv/echecs (non retentés : XenosRecomp plante sur les 64 VS XDK du décodeur WMV).
void natifShaders(const Install& in)
{
    const fs::path S = in.D / "spirv", C = S / "conteneurs", E = S / "echecs", N = in.T / "natif";
    fs::create_directories(C);
    fs::create_directories(E);
#ifdef _WIN32
    const fs::path dxc = N / "dxc.exe";
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);   // hérité : pas de fenêtre d'erreur si XenosRecomp plante
#else
    const fs::path dxc = N / "dxc" / "bin" / "dxc";
    setenv("LD_LIBRARY_PATH", (N / "dxc" / "lib").c_str(), 1);   // libdxcompiler.so (DXC et XenosRecomp)
#endif
    if (!fs::exists(C / ".extrait"))
    {
        for (auto& e : fs::directory_iterator(in.G / "base" / "Media" / "Shaders"))
            if (e.path().extension() == ".xpu" || e.path().extension() == ".xvu")
                fs::copy_file(e.path(), C / e.path().filename(), fs::copy_options::overwrite_existing);
        step("extraction des shaders du XEX", { w(exe(N / "xr_hash")), A("-x"), w(in.G / "patched" / "image.bin"), w(C) });
        std::ofstream(C / ".extrait").close();
    }
    int ok = 0, ko = 0, deja = 0;
    for (auto& e : fs::directory_iterator(C))
    {
        std::ifstream f(e.path(), std::ios::binary);
        std::vector<unsigned char> b{ std::istreambuf_iterator<char>(f), {} };
        auto be = [&](size_t o) { return uint32_t(b[o]) << 24 | uint32_t(b[o + 1]) << 16 | uint32_t(b[o + 2]) << 8 | b[o + 3]; };
        if (b.size() < 36 || (be(0) & 0xFFFFFF00u) != 0x102A1100u) continue;
        const size_t taille = size_t(be(4)) + be(8);
        if (taille > b.size()) continue;   // conteneur tronqué
        char h[17];
        snprintf(h, sizeof h, "%016llx", (unsigned long long)XXH3_64bits(b.data(), taille));
        const bool vs = be(0) & 1;
        if (fs::exists(S / (std::string(h) + ".spv")) || fs::exists(E / h)) { ++deja; continue; }
        const fs::path hlsl = S / (std::string(h) + ".hlsl"), spv = S / (std::string(h) + ".spv.tmp");
        int rc = runProcess({ w(exe(N / "XenosRecomp")), w(e.path()), w(hlsl), w(N / "shader_common.h") });
        const bool xr = rc == 0 && fs::exists(hlsl) && fs::file_size(hlsl) > 0;
        if (xr)   // mêmes options que precompile-shaders.sh (XenosRecomp/dxc_compiler.cpp, chemin SPIR-V)
        {
            std::vector<Arg> a = { w(dxc), vs ? A("-Tvs_6_0") : A("-Tps_6_0"), A("-HV"), A("2021"), A("-all-resources-bound"),
                A("-spirv"), A("-fvk-use-dx-layout"), A("-Qstrip_debug"), A("-E"), A("main"), A("-Fo"), w(spv), w(hlsl) };
            if (vs) a.insert(a.begin() + 7, A("-fvk-invert-y"));
            rc = runProcess(a);
        }
        std::error_code ec;
        fs::remove(hlsl, ec);
        if (xr && rc == 0) { fs::rename(spv, S / (std::string(h) + ".spv")); ++ok; }
        else { fs::remove(spv, ec); std::ofstream(E / h) << u8(e.path().filename()) << (xr ? " dxc " : " xenosrecomp ") << rc << "\n"; ++ko; }
    }
    say("[natif] shaders : %d nouveaux en SPIR-V, %d échecs, %d déjà traités (%s)\n", ok, ko, deja, u8(S).c_str());
}

// GPU Vulkan de la machine (lecture locale ; chargeur Vulkan ouvert à la demande, comme le jeu) ; vide sans Vulkan.
std::vector<wk::config::GpuVu> gpusVulkan()
{
    std::vector<wk::config::GpuVu> r;
#ifdef _WIN32
    HMODULE m = LoadLibraryW(L"vulkan-1.dll");
    struct Fermer { HMODULE m; ~Fermer() { if (m) FreeLibrary(m); } } fermer{ m };
    auto gipa = m ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void*>(GetProcAddress(m, "vkGetInstanceProcAddr"))) : nullptr;
#else
    void* m = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    struct Fermer { void* m; ~Fermer() { if (m) dlclose(m); } } fermer{ m };
    auto gipa = m ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(m, "vkGetInstanceProcAddr")) : nullptr;
#endif
    auto creer = gipa ? reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance")) : nullptr;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    if (!creer || creer(&ci, nullptr, &inst) != VK_SUCCESS) return r;
    auto lister = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(inst, "vkEnumeratePhysicalDevices"));
    auto proprietes = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(inst, "vkGetPhysicalDeviceProperties"));
    auto detruire = reinterpret_cast<PFN_vkDestroyInstance>(gipa(inst, "vkDestroyInstance"));
    uint32_t n = 0;
    if (lister && proprietes && lister(inst, &n, nullptr) == VK_SUCCESS) {
        std::vector<VkPhysicalDevice> d(n);
        if (lister(inst, &n, d.data()) == VK_SUCCESS)
            for (VkPhysicalDevice p : d) {
                VkPhysicalDeviceProperties pr;
                proprietes(p, &pr);
                r.push_back({ pr.deviceName, int(pr.deviceType), pr.apiVersion >= VK_API_VERSION_1_3 });
            }
    }
    if (detruire) detruire(inst, nullptr);
    return r;
}

// Journal du lanceur : GPU que le jeu choisira (même règle, config::ChoisirGpu) et multi-échantillonnage retenu.
// Rend les options lues (défauts si config.toml est absent).
wk::config::Config journalGpu(const fs::path& data)
{
    wk::config::Config c;
    std::ifstream f(data / "config.toml");
    if (f) {
        std::stringstream ss;
        ss << f.rdbuf();
        std::vector<std::string> err;
        c = wk::config::Parse(ss.str(), &err);
    }
    static const char* const kTypes[] = { "autre", "intégré", "dédié", "virtuel", "processeur" };
    auto type = [](int t) { return t >= 0 && t <= 4 ? kTypes[t] : "?"; };
    const auto gpus = gpusVulkan();
    for (const auto& g : gpus) say("[gpu] vu : %s (%s%s)\n", g.nom.c_str(), type(g.type), g.vulkan13 ? "" : ", sans Vulkan 1.3");
    bool trouve = true;
    const int i = wk::config::ChoisirGpu(gpus, c.gpu, &trouve);
    if (i < 0) { say("[gpu] aucun GPU Vulkan 1.3 détecté\n"); return c; }
    const bool msaa = wk::config::MsaaActif(c.msaa, gpus[i].type);
    say("[gpu] retenu : %s (%s, %s) ; multi-échantillonnage %s (%s)\n", gpus[i].nom.c_str(), type(gpus[i].type),
        c.gpu.empty() ? "choix automatique" : trouve ? "option gpu" : "option gpu introuvable, choix automatique",
        msaa ? "activé" : "désactivé",
        c.msaa != "auto" ? "choix du joueur" : gpus[i].type == 2 ? "auto : GPU dédié" : "auto : pas de GPU dédié");
    return c;
}

int launchGame(const Install& in)
{
    const fs::path& D = in.D, g = D / "work" / "game";
    const fs::path data = dataDir(D);
    const bool console = journalGpu(data).console;
    natifShaders(in);   // conteneurs déposés par le jeu au lancement précédent
    const fs::path N = in.T / "natif";   // outils pour traduire au vol les shaders créés par le jeu (gardés par menage)
#ifdef _WIN32
    SetEnvironmentVariableW(L"WAKFU_NATIF_SPIRV", w(D / "spirv").c_str());
    SetEnvironmentVariableW(L"XENOSRECOMP", w(N / "XenosRecomp.exe").c_str());
    SetEnvironmentVariableW(L"XENOS_INCLUDE", w(N / "shader_common.h").c_str());
    SetEnvironmentVariableW(L"DXC", w(N / "dxc.exe").c_str());
#else
    setenv("WAKFU_NATIF_SPIRV", (D / "spirv").c_str(), 1);
    setenv("XENOSRECOMP", (N / "XenosRecomp").c_str(), 1);
    setenv("XENOS_INCLUDE", (N / "shader_common.h").c_str(), 1);
    setenv("DXC", (N / "dxc" / "bin" / "dxc").c_str(), 1);
    setenv("DXC_LIB", (N / "dxc" / "lib").c_str(), 1);
    setenv("WAKFU_JOURNAL", "1", 1);   // stderr du jeu vers <données>/journal.log (runtime, DetachConsole)
#endif
    say("Lancement : %s (données et journal : %s)\n", u8(exe(D / "wakfu")).c_str(), u8(data).c_str());
    return runProcess({ w(exe(D / "wakfu")), w(g / "patched" / "image.bin"), w(g / "patched" / "default.xex"),
                        w(g / "base"), w(g / "tu2"), w(data) }, D, true, !console);
}

#ifndef _WIN32
bool dansPath(const std::string& c)
{
    std::string p = envPath("PATH").native();
    for (size_t a = 0; a <= p.size();)
    {
        size_t b = p.find(':', a);
        if (b == std::string::npos) b = p.size();
        if (b > a && access((p.substr(a, b - a) + "/" + c).c_str(), X_OK) == 0) return true;
        a = b + 1;
    }
    return false;
}
// Paquets Arch / CachyOS (installer.sh) ; glslang : fichier de configuration CMake (pas toujours de .pc)
const char* const kPaquets[] = { "clang", "cmake", "ninja", "pkgconf", "sdl2-compat", "vulkan-headers", "vulkan-icd-loader", "glslang", "ffmpeg" };
#endif

std::vector<std::string> dependancesManquantes()
{
    std::vector<std::string> r;
#ifndef _WIN32
    for (const char* c : { "clang", "clang++", "cmake", "ninja", "pkg-config" })
        if (!dansPath(c)) r.push_back(c);
    if (dansPath("pkg-config"))
        for (const char* m : { "sdl2", "vulkan", "libavcodec", "libavutil" })
            if (std::system(("pkg-config --exists " + std::string(m)).c_str()) != 0) r.push_back(m);
    bool glslang = false;
    for (const char* d : { "/usr/lib/cmake/glslang", "/usr/lib64/cmake/glslang", "/usr/lib/x86_64-linux-gnu/cmake/glslang", "/usr/local/lib/cmake/glslang" })
        glslang |= fs::exists(fs::path(d) / "glslangConfig.cmake") || fs::exists(fs::path(d) / "glslang-config.cmake");
    if (!glslang) r.push_back("glslang");
#endif
    return r;
}

bool pacmanDisponible()
{
#ifdef _WIN32
    return false;
#else
    return dansPath("pacman") && dansPath("pkexec");
#endif
}

void installerDependances()
{
#ifndef _WIN32
    std::vector<Arg> a = { "pkexec", "pacman", "-S", "--needed", "--noconfirm" };
    for (const char* p : kPaquets) a.push_back(p);
    say("Installation des dépendances (pacman)\n");
    step("installation des dépendances", a);
#endif
}

// 1. Outils
void ensureTools(Install& in)
{
    etape(EtOutils);
    const fs::path& D = in.D, &T = in.T;
#ifndef _WIN32
    if (auto m = dependancesManquantes(); !m.empty())
    {
        std::string l;
        for (auto& x : m) l += (l.empty() ? "" : ", ") + x;
        throw Error("dépendances manquantes : " + l);
    }
#endif
    if (!fs::exists(T / ".complet"))
    {
        say("[1/5] Extraction des outils dans %s\n", u8(T).c_str());
        fs::path tmp = D / ("outils-" + in.id + ".tmp");
        supprimerDans(D, tmp);
        supprimerDans(D, T);
        extractPayload(selfPath(), Trailer{ in.off, in.size, in.id }, tmp);
#ifndef _WIN32
        fs::permissions(tmp / "natif" / "dxc" / "bin" / "dxc", fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec, fs::perm_options::add);
#endif
        std::ofstream(tmp / ".complet").close();
        fs::rename(tmp, T);
        for (auto& e : fs::directory_iterator(D))   // anciennes versions des outils (outils-<16 chiffres hexa>)
        {
            const std::string n = u8(e.path().filename());
            if (e.is_directory() && e.path() != T && n.size() == 23 && n.rfind("outils-", 0) == 0) supprimerDans(D, e.path());
        }
    }
    else say("[1/5] Outils déjà extraits : %s\n", u8(T).c_str());
#ifdef _WIN32
    std::wstring path = w(T / "llvm-mingw" / "bin") + L";" + w(T / "bin") + L";" + w(T / "cmake" / "bin") + L";" + envPath(L"PATH").wstring();
    SetEnvironmentVariableW(L"PATH", path.c_str());
#else
    // Outils compilés ici depuis les sources de l'archive (première fois, ou après le ménage)
    if (!fs::exists(T / ".construit"))
    {
        say("[1/5] Compilation des outils (wakfu-prepare, XenonRecomp, XenosRecomp)\n");
        const fs::path bo = T / "build-outils", bs = T / "build-shaders";
        propre(D, T / "src" / "tools-src" / "wakfu-prepare", bo);
        step("configuration des outils", { "cmake", "-S", w(T / "src" / "tools-src" / "wakfu-prepare"), "-B", w(bo), "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++", wdef("XENONRECOMP_DIR", T / "xenonrecomp") });
        step("compilation des outils", { "ninja", "-C", w(bo), "wakfu-prepare", "XenonRecomp" });
        propre(D, T / "src" / "packaging" / "shaders" / "xenosrecomp", bs);
        step("configuration de XenosRecomp", { "cmake", "-S", w(T / "src" / "packaging" / "shaders" / "xenosrecomp"), "-B", w(bs), "-G", "Ninja",
            "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++",
            wdef("XENOSRECOMP_DIR", T / "xenosrecomp"), wdef("DXC_DIR", T / "natif" / "dxc") });
        step("compilation de XenosRecomp", { "ninja", "-C", w(bs), "XenosRecomp", "xr_hash" });
        fs::create_directories(in.bin);
        const auto o = fs::copy_options::overwrite_existing;
        fs::copy_file(bo / "wakfu-prepare", in.bin / "wakfu-prepare", o);
        fs::copy_file(bo / "xenonrecomp" / "XenonRecomp" / "XenonRecomp", in.bin / "XenonRecomp", o);
        fs::copy_file(bs / "XenosRecomp" / "XenosRecomp", T / "natif" / "XenosRecomp", o);
        fs::copy_file(bs / "xr_hash", T / "natif" / "xr_hash", o);
        fs::copy_file(T / "xenosrecomp" / "XenosRecomp" / "shader_common.h", T / "natif" / "shader_common.h", o);
        std::ofstream(T / ".construit").close();
    }
#endif
}

// 2. Jeu
bool prepareGame(Install& in, const std::vector<fs::path>& inputs)
{
    etape(EtPreparation);
    const fs::path& G = in.G;
    if (!inputs.empty())
    {
        for (auto& i : inputs)   // jamais d'entrée dans work/game, vidé ci-dessous
            if (isInside(i, in.D)) throw Error("l'entrée " + u8(i) + " est dans le dossier de travail " + u8(in.D) + " : la déplacer");
        say("[2/5] Préparation du jeu (extraction, mise à jour)\n");
        supprimerDans(in.D, G);
        fs::remove(in.W / "out" / ".recomp-ok");
        std::vector<Arg> a = { w(exe(in.bin / "wakfu-prepare")), A("-o"), w(G) };
        for (auto& i : inputs) a.push_back(w(fs::absolute(i)));
        // Code 3 : jeu préparé, SHA-1 de l'image non reconnu (wakfu-prepare a écrit son avertissement ; la ligne de
        // commande continue comme avant, la fenêtre refuse la copie)
        return step("préparation du jeu", a, {}, 3) != 3;
    }
    say("[2/5] Jeu déjà préparé : %s\n", u8(G).c_str());
    return true;
}

void buildGame(Install& in)
{
    const fs::path &D = in.D, &W = in.W, &G = in.G, &T = in.T, &bin = in.bin, sdk = T / kSdk;
    // 3. C++ recompilé (XenonRecomp écrit dans work/out/ppc d'après config/wakfu.toml)
    fs::path recompDone = W / "out" / ".recomp-ok";
    etape(EtXenon);
    if (!isUpToDate(in) || !fs::exists(W / "out" / "ppc"))   // jamais fait, jeu re-préparé, autre version des outils, ménage
    {
        say("[3/5] Recompilation du code PowerPC (XenonRecomp)\n");
        supprimerDans(D, D / "config");
        fs::copy(T / "src" / "config", D / "config", fs::copy_options::recursive);
        supprimerDans(D, W / "out" / "ppc");
        fs::create_directories(W / "out" / "ppc");
        step("XenonRecomp", { w(exe(bin / "XenonRecomp")), w(D / "config" / "wakfu.toml"), w(sdk / "XenonUtils" / "ppc_context.h") }, D / "config");
        etape(EtImports);
        say("  %s\n", genImports(G / "patched" / "default.xex", G / "patched" / "patched.txt", sdk / "XenonUtils" / "xbox", W / "gen").c_str());
        std::ofstream(recompDone) << in.id;
    }
    else say("[3/5] C++ déjà généré\n");

    // 4. Compilation
    // CMake est lancé depuis W : sous Wine, un dossier courant sur Z: (racine Unix) fait échouer la génération
    // (chemins de sources corrompus, observé avec CMake 3.31.6).
    etape(EtConfiguration);
    fs::path build = W / "build";
    {
        std::string id;
        std::ifstream(build / ".outils") >> id;
        if (id != in.id) supprimerDans(D, build);   // autre version des outils : configuration à refaire
    }
    propre(D, T / "src" / "runtime", build);
    if (!fs::exists(build / ".outils"))   // écrit seulement après une configuration réussie
    {
        supprimerDans(D, build);
        say("[4/5] Configuration (CMake)\n");
#ifdef _WIN32
        fs::path L = T / "llvm-mingw" / "bin";
        // Icône de wakfu.exe tirée du jeu du joueur ; sans elle, le jeu se compile quand même
        const fs::path ico = W / "wakfu.ico";
        const bool icone = PngVersIco(u8(G / "base" / "TitleIcon.png"), u8(ico));
        if (!icone) say("Icône du jeu non créée (TitleIcon.png absent ou illisible) : wakfu.exe sans icône.\n");
        step("configuration CMake", { w(T / "cmake" / "bin" / "cmake.exe"), L"-S", w(T / "src" / "runtime"), L"-B", w(build), L"-G", L"Ninja",
            wdef("CMAKE_MAKE_PROGRAM", bin / "ninja.exe"), L"-DCMAKE_BUILD_TYPE=Release",
            wdef("CMAKE_C_COMPILER", L / "clang.exe"), wdef("CMAKE_CXX_COMPILER", L / "clang++.exe"),
            wdef("PPC_DIR", W / "out" / "ppc"), wdef("SIMDE_DIR", sdk / "thirdparty" / "simde"), wdef("GEN_DIR", W / "gen"),
            wdef("glslang_DIR", T / "deps" / "glslang" / "lib" / "cmake" / "glslang"),
            wdef("SDL2_DIR", T / "deps" / "SDL2" / "lib" / "cmake" / "SDL2"),
            wdef("FFMPEG_PREFIX", T / "deps" / "ffmpeg"), wdef("VULKAN_SYS_INCLUDE", T / "deps" / "vulkan" / "include"),
            wdef("CMAKE_RC_COMPILER", L / "x86_64-w64-mingw32-windres.exe"), wdef("GAME_ICON", icone ? ico : fs::path()) }, W);
#else
        step("configuration CMake", { "cmake", "-S", w(T / "src" / "runtime"), "-B", w(build), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++",
            wdef("PPC_DIR", W / "out" / "ppc"), wdef("SIMDE_DIR", sdk / "thirdparty" / "simde"), wdef("GEN_DIR", W / "gen") }, W);
#endif
        std::ofstream(build / ".outils") << in.id;
    }
    say("[4/5] Compilation du jeu (long : plusieurs minutes à une heure selon le processeur)\n");
    etape(EtCompilation);   // après « [4/5] », qui ressemble à une ligne de ninja « [n/N] » (progression)
#ifdef _WIN32
    const fs::path cmake = T / "cmake" / "bin" / "cmake.exe";
#else
    const fs::path cmake = "cmake";
#endif
    step("compilation", { w(cmake), A("--build"), w(build), A("--target"), A("wakfu") }, W);

    // 5. Installation : jeu, shaders du jeu en SPIR-V
    etape(EtInstallation);
    fs::copy_file(exe(build / "wakfu"), exe(D / "wakfu"), fs::copy_options::overwrite_existing);
    natifShaders(in);
#ifdef _WIN32
    std::ofstream(D / "jouer.cmd", std::ios::binary)
        << "@echo off\r\ncd /d \"%~dp0\"\r\nset \"WAKFU_NATIF_SPIRV=%~dp0spirv\"\r\n"
           "set \"XENOSRECOMP=%~dp0outils-" << in.id << "\\natif\\XenosRecomp.exe\"\r\n"
           "set \"XENOS_INCLUDE=%~dp0outils-" << in.id << "\\natif\\shader_common.h\"\r\n"
           "set \"DXC=%~dp0outils-" << in.id << "\\natif\\dxc.exe\"\r\n"
           "\"%~dp0wakfu.exe\" \"%~dp0work\\game\\patched\\image.bin\" \"%~dp0work\\game\\patched\\default.xex\" "
           "\"%~dp0work\\game\\base\" \"%~dp0work\\game\\tu2\" \"" << (dataDir(D) == D / "userdata" ? "%~dp0userdata" : "%APPDATA%\\WakfuRecomp") << "\"\r\n";
#endif
    say("[5/5] Prêt : %s\n  Pour jouer : relancer WakfuRecomp sans argument\n", u8(exe(D / "wakfu")).c_str());
}

// Ménage (décision 19) : gardés : jeu (wakfu.exe), jeu préparé (work/game), config, spirv, userdata, outils des shaders
std::vector<fs::path> menage(const Install& in)
{
    std::vector<fs::path> c = { in.W / "out" / "ppc", in.W / "gen", in.W / "build" }, r;
    std::error_code ec;
    for (auto it = fs::directory_iterator(in.T, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->path().filename() != "natif") c.push_back(it->path());   // .complet compris : réextraction au besoin
    for (auto& p : c)
        if (supprimerDans(in.D, p)) { r.push_back(p); say("ménage : %s supprimé\n", u8(p).c_str()); }
    return r;
}

// ---- Raccourcis ----
fs::path raccourciMenu()
{
#ifdef _WIN32
    PWSTR p = nullptr;
    fs::path r;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &p))) r = fs::path(p) / "WakfuRecomp.lnk";
    CoTaskMemFree(p);
    return r;
#else
    return dataHome() / "applications" / "WakfuRecomp.desktop";
#endif
}

fs::path raccourciBureau()
{
#ifdef _WIN32
    PWSTR p = nullptr;
    fs::path r;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p))) r = fs::path(p) / "WakfuRecomp.lnk";
    CoTaskMemFree(p);
    return r;
#else
    // XDG_DESKTOP_DIR="$HOME/Bureau" dans ~/.config/user-dirs.dirs, sinon ~/Desktop
    const fs::path h = envPath("HOME");
    fs::path conf = envPath("XDG_CONFIG_HOME");
    if (!conf.is_absolute()) conf = h / ".config";
    std::ifstream f(conf / "user-dirs.dirs");
    fs::path d = h / "Desktop";
    for (std::string l; std::getline(f, l);)
        if (l.rfind("XDG_DESKTOP_DIR=\"", 0) == 0 && l.size() > 18 && l.back() == '"')
        {
            std::string v = l.substr(17, l.size() - 18);
            d = v.rfind("$HOME", 0) == 0 ? fs::path(h.native() + v.substr(5)) : fs::path(v);
        }
    return d / "WakfuRecomp.desktop";
#endif
}

// Raccourci vers ce lanceur (appelé depuis le fil de la fenêtre : COM initialisé)
void creerRaccourci(const fs::path& lien, const Install& in)
{
    if (lien.empty()) throw Error("dossier du raccourci introuvable");
    fs::create_directories(lien.parent_path());
#ifdef _WIN32
    (void)in;
    IShellLinkW* l = nullptr;
    IPersistFile* f = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&l));
    if (SUCCEEDED(hr)) hr = l->SetPath(selfPath().c_str());
    if (SUCCEEDED(hr)) hr = l->SetWorkingDirectory(selfPath().parent_path().c_str());
    if (SUCCEEDED(hr)) hr = l->SetDescription(L"Islands of Wakfu (WakfuRecomp)");
    if (SUCCEEDED(hr)) hr = l->QueryInterface(IID_PPV_ARGS(&f));
    if (SUCCEEDED(hr)) hr = f->Save(lien.c_str(), TRUE);
    if (f) f->Release();
    if (l) l->Release();
    if (FAILED(hr)) throw Error("création du raccourci impossible : " + u8(lien));
#else
    std::string e;   // Exec : chemin entre guillemets, « " ` $ \ » échappés (Desktop Entry Specification)
    const fs::path self = selfPath();
    for (char ch : self.native())
    {
        if (std::strchr("\"`$\\", ch)) e += '\\';
        e += ch;
    }
    {
        std::ofstream o(lien, std::ios::trunc);
        o << "[Desktop Entry]\nType=Application\nName=WakfuRecomp\nComment=Islands of Wakfu\nExec=\"" << e << "\"\n"
          << "Icon=" << (in.G / "base" / "TitleIcon.png").native() << "\nTerminal=false\nCategories=Game;\n";
        if (!o.flush()) throw Error("création du raccourci impossible : " + u8(lien));
    }
    fs::permissions(lien, fs::perms::owner_exec, fs::perm_options::add);   // bureau : lanceur exécutable
#endif
}

namespace {

void supprimerFichier(const fs::path& p)   // fichier seul (raccourci) : jamais un dossier ni un lien suivi
{
    std::error_code ec;
    auto s = fs::symlink_status(p, ec);
    if (fs::is_regular_file(s) || fs::is_symlink(s)) fs::remove(p);
}

bool contientLanceur(const fs::path& p)
{
    std::error_code ec;
    auto r = fs::weakly_canonical(selfPath(), ec).lexically_relative(fs::weakly_canonical(p, ec));
    return !r.empty() && *r.begin() != "..";
}

} // namespace

std::vector<fs::path> aDesinstaller(const Install& in, bool garder)
{
    std::vector<fs::path> r;
    std::error_code ec;
    for (auto it = fs::directory_iterator(in.D, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
    {
        const fs::path n = it->path().filename();
        if (garder && (n == "userdata" || n == ".wakfurecomp")) continue;   // marqueur gardé avec les sauvegardes
        if (!contientLanceur(it->path())) r.push_back(it->path());
    }
    std::vector<fs::path> donnees = { in.D / "userdata" };
    if (fs::path a = appDataDir(); !a.empty() && fs::is_directory(a, ec)) donnees.push_back(a);
    for (auto& d : donnees)
    {
        if (!fs::is_directory(d, ec) || (d == in.D / "userdata" && !garder)) continue;
        if (!garder) { if (!contientLanceur(d)) r.push_back(d); continue; }
        for (const char* j : { "journal.log", "journal.1.log", "journal.2.log", "cache" })   // journaux et cache de shaders du jeu, même si sauvegardes gardées
            if (fs::exists(d / j, ec)) r.push_back(d / j);
    }
    for (const fs::path& l : { raccourciMenu(), raccourciBureau() })
        if (!l.empty() && fs::exists(fs::symlink_status(l, ec))) r.push_back(l);
    return r;
}

void desinstaller(const Install& in, bool garder)
{
    const fs::path a = appDataDir();
    std::vector<fs::path> gardes;   // fichiers d'entrée (ISO, STFS, TU) rangés là par le joueur : jamais supprimés
    for (auto& p : aDesinstaller(in, garder))
    {
        if (isInside(p, in.D)) supprimerDans(in.D, p, &gardes);
        else if (!a.empty() && (p == a || isInside(p, a))) supprimerDans(a, p, &gardes);
        else supprimerFichier(p);   // raccourcis
    }
    for (auto& g : gardes) say("fichier d'entrée gardé : %s\n", u8(g).c_str());
    std::error_code ec;
    fs::remove(in.D, ec);   // seulement s'il est vide (sauvegardes non gardées)
}

// ---- Anciennes installations (décision 21) ----
std::vector<Ancienne> anciennesInstallations()
{
    std::vector<Ancienne> r;
    std::error_code ec;
    auto ajouter = [&](const fs::path& d, std::vector<fs::path> f = {}) {
        if (!fs::is_directory(fs::symlink_status(d, ec)) || d == defaultWorkDir()) return;
        r.push_back({ d, std::move(f), tailleSansLiens(d) });
    };
#ifdef _WIN32
    if (fs::path la = envPath(L"LOCALAPPDATA"); !la.empty()) ajouter(la / "WakfuRecomp-natif");
#else
    const fs::path b = dataHome();
    ajouter(b / "wakfu-recomp-natif");   // essai du rendu natif (essayer-natif.sh)
    // Émulation (installer.sh) dans le dossier du nouveau lanceur : sans marqueur .wakfurecomp
    const fs::path e = b / "wakfu-recomp";
    if (fs::is_directory(fs::symlink_status(e, ec)) && !fs::exists(e / ".wakfurecomp") && (fs::exists(e / "jouer.sh") || fs::exists(e / "work")))
        r.push_back({ e, { b / "applications" / "wakfu-recomp.desktop" }, tailleSansLiens(e) });
#endif
    return r;
}

void supprimerAncienne(const Ancienne& a)
{
    say("ancienne installation supprimée : %s\n", u8(a.dossier).c_str());
    std::vector<fs::path> gardes;
    supprimerDans(a.dossier, a.dossier, &gardes);
    for (auto& g : gardes) say("fichier d'entrée gardé : %s\n", u8(g).c_str());
    for (auto& f : a.fichiers) supprimerFichier(f);
}

void quitterDeSuite()
{
#ifdef _WIN32
    ExitProcess(1);   // le job arrête les outils avec nous
#else
    if (pid_t p = g_enfant; p > 0) kill(-p, SIGKILL);
    _exit(1);
#endif
}

#ifdef _WIN32
void createJob()   // fenêtre : les outils (et leurs enfants : ninja, clang) s'arrêtent avec le lanceur
{
    g_job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (g_job) SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof li);
}
#endif

namespace {

// Empreinte des entrées : chemin absolu, taille et date de chaque fichier (pour un dossier : de chaque fichier
// qu'il contient, sur 4 niveaux comme wakfu-prepare). Vide si une entrée est illisible.
std::string inputsStamp(const std::vector<fs::path>& inputs)
{
    std::ostringstream s;
    auto add = [&](const fs::path& p) {
        std::error_code ec;
        auto size = fs::file_size(p, ec);
        auto t = static_cast<long long>(fs::last_write_time(p, ec).time_since_epoch().count());   // __int128 sous libc++ MinGW
        s << u8(fs::absolute(p)) << '|' << size << '|' << t << '\n';
    };
    try
    {
        for (auto& i : inputs)
        {
            if (!fs::is_directory(i)) { add(i); continue; }
            std::vector<fs::path> files;
            for (auto it = fs::recursive_directory_iterator(i, fs::directory_options::skip_permission_denied);
                 it != fs::recursive_directory_iterator(); ++it)
            {
                if (it.depth() >= 4) it.disable_recursion_pending();
                if (it->is_regular_file()) files.push_back(it->path());
            }
            std::sort(files.begin(), files.end());
            for (auto& f : files) add(f);
        }
    }
    catch (const std::exception&) { return {}; }
    return s.str();
}

// RecompLauncher (RECOMPLAUNCHER=1) : lignes de l'installateur et des outils sur stdout et dans
// <données>/journal-installation.log, progression « progress <0..1> <étape> » par tranches fixes.
class SuiviRecompLauncher : public Suivi
{
public:
    void etape(Etape e) override { e_ = e; progression(0, true); }
    void avance(double f) override { progression(f, false); }
    void ligne(const std::string& s) override
    {
        for (size_t a = 0; a < s.size();)   // une ligne par morceau terminé par \n ou \r
        {
            size_t b = s.find_first_of("\r\n", a);
            if (b == std::string::npos) b = s.size();
            if (b > a) ecrire(s.substr(a, b - a));
            a = b + 1;
        }
    }
    void ouvrir(const fs::path& p)   // journal tronqué à chaque lancement ; lignes déjà écrites recopiées
    {
#ifdef _WIN32
        journal_ = _wfopen(p.c_str(), L"wb");
#else
        journal_ = std::fopen(p.c_str(), "wb");
#endif
        if (!journal_) { ecrire("journal impossible à créer : " + u8(p)); return; }
        std::fputs(avant_.c_str(), journal_);
        std::fflush(journal_);
        avant_.clear();
    }

private:
    Etape e_ = EtOutils;
    double dernier_ = -1;
    FILE* journal_ = nullptr;
    std::string avant_;   // lignes écrites avant l'ouverture du journal

    void ecrire(const std::string& l)
    {
        std::printf("%s\n", l.c_str());
        std::fflush(stdout);
        if (journal_) { std::fprintf(journal_, "%s\n", l.c_str()); std::fflush(journal_); }
        else if (avant_.size() < 65536) avant_ += l + "\n";
        unsigned n, N;   // ninja : « [n/N] Building CXX object ... »
        if (e_ == EtCompilation && l[0] == '[' && std::sscanf(l.c_str(), "[%u/%u]", &n, &N) == 2 && N > 0) progression(double(n) / N, false);
        float pc;   // XenonRecomp : « Recompiling functions... 13.4% »
        if (e_ == EtXenon && std::sscanf(l.c_str(), "Recompiling functions... %f%%", &pc) == 1) progression(pc / 100.0, false);
    }
    void progression(double f, bool toujours)
    {
        // outils 0-0,05 ; préparation 0,05-0,15 ; XenonRecomp (et imports) 0,15-0,3 ; configuration et compilation 0,3-1
        static const double kDe[NbEtapes] = { 0, 0.05, 0.15, 0.3, 0.3, 0.3, 1 }, kA[NbEtapes] = { 0.05, 0.15, 0.3, 0.3, 0.3, 1, 1 };
        static const char* const kNom[NbEtapes] = { "Outils", "Préparation du jeu", "Recompilation du code (XenonRecomp)",
            "Imports", "Configuration", "Compilation", "Installation" };
        const double v = kDe[e_] + std::clamp(f, 0.0, 1.0) * (kA[e_] - kDe[e_]);
        if (!toujours && v < dernier_ + 0.001) return;   // au plus ~1000 lignes ; jamais en arrière dans une étape
        dernier_ = v;
        char b[32];
        std::snprintf(b, sizeof b, "progress %.3f ", v);   // point décimal : locale « C » (setlocale jamais appelé ici)
        ecrire(b + std::string(kNom[e_]));
    }
};
SuiviRecompLauncher g_suiviRL;

int realMain(const std::vector<fs::path>& args)
{
    fs::path D;
    bool launch = true;
    std::vector<fs::path> inputs;
    for (size_t i = 0; i < args.size(); ++i)
    {
        const fs::path& a = args[i];
        if (a == "--dossier" && i + 1 < args.size()) D = args[++i];
        else if (a == "--sans-lancer") launch = false;
        else if (a == "-h" || a == "--help" || a == "/?" || a == "--dossier") return usage();
        else inputs.push_back(a);
    }
    Install in;
    openWorkDir(in, D.empty() ? defaultWorkDir() : D);
    D = in.D;
    if (g_recompLauncher) g_suiviRL.ouvrir(dataDir(D) / "journal-installation.log");
    bool prepared = isPrepared(in);

    if (inputs.empty() && prepared && fs::exists(exe(D / "wakfu"))) return launch ? launchGame(in) : 0;
    if (inputs.empty() && !prepared)
    {
        usage();
#ifdef _WIN32
        if (!g_recompLauncher)   // RecompLauncher : jamais d'attente
        {
        say("\nChemin(s) du jeu et de la mise à jour (glisser les fichiers dans cette fenêtre), puis Entrée :\n> ");
        inputs = splitPaths(readLine());
        }
#endif
        if (inputs.empty()) return 2;
    }
    // Mêmes entrées que la dernière préparation réussie (RecompLauncher repasse le dossier du jeu à chaque
    // lancement) : pas de nouvelle préparation ; outils et compilation restent vérifiés ci-dessous.
    fs::path stampFile = in.W / "entrees.txt";
    std::string stamp = inputsStamp(inputs), oldStamp;
    {
        std::ifstream f(stampFile, std::ios::binary);
        oldStamp.assign(std::istreambuf_iterator<char>(f), {});
    }
    if (!inputs.empty() && prepared && !stamp.empty() && stamp == oldStamp)
    {
        say("Jeu déjà préparé depuis ces fichiers.\n");
        inputs.clear();
    }
    if (!inputs.empty()) verifierEntrees(inputs);
    for (auto& i : inputs)
        if (isInside(i, D)) throw Error("l'entrée " + u8(i) + " est dans le dossier de travail " + u8(D) + " : la déplacer");

    double gb = freeGb(D);
    if (gb >= 0 && gb < 3)
        say("ATTENTION : %.1f Go libres sur le disque de %s (~3 Go nécessaires).\n", gb, u8(D).c_str());

    ensureTools(in);
    if (!inputs.empty())
    {
        fs::remove(stampFile);
        if (prepareGame(in, inputs)) std::ofstream(stampFile, std::ios::binary) << stamp;
    }
    else prepareGame(in, inputs);
    buildGame(in);
    return launch ? launchGame(in) : 0;
}

int principal(const std::vector<fs::path>& args)
{
    if (g_recompLauncher) g_suivi = &g_suiviRL;
    int rc;
    try { rc = realMain(args); }
    catch (const std::exception& e)
    {
        say("\nERREUR : %s\n", e.what());
        rc = 1;
    }
    return rc;
}

} // namespace
} // namespace wk

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
    if (argc <= 1)   // double-clic : fenêtre du lanceur
    {
        wk::createJob();
        return wk::guiMain();
    }
    if (wk::g_recompLauncher)
    {
        // RecompLauncher : ni console ni attente ; sorties standard héritées (si valides), outils arrêtés avec nous,
        // journal du jeu toujours écrit (journal.log, joint aux rapports de plantage)
        wk::createJob();
        SetEnvironmentVariableW(L"WAKFU_JOURNAL", L"1");
        return wk::principal(std::vector<wk::fs::path>(argv + 1, argv + argc));
    }
    wk::openConsole();
    SetConsoleOutputCP(CP_UTF8);
    int rc = wk::principal(std::vector<wk::fs::path>(argv + 1, argv + argc));
    if (wk::g_ownConsole && rc != 0)
    {
        wk::say("\nAppuyer sur Entrée pour fermer.");
        wk::readLine();
    }
    return rc;
}
#else
int main(int argc, char** argv)
{
    if (argc <= 1) return wk::guiMain();
    return wk::principal(std::vector<wk::fs::path>(argv + 1, argv + argc));
}
#endif
