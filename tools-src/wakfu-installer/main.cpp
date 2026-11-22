// WakfuRecomp.exe : installateur Windows en un seul fichier.
// 1. extrait l'archive ajoutée à la fin de l'exécutable (outils : llvm-mingw, CMake, ninja, XenonRecomp, wakfu-prepare,
//    sources du runtime, bibliothèques) dans %LOCALAPPDATA%\WakfuRecomp\outils-<id> (une fois par version) ;
// 2. prépare le jeu de l'utilisateur (wakfu-prepare : STFS / ISO-XISO / dossier + TU) ;
// 3. génère le C++ (XenonRecomp patché) et les stubs d'imports ; 4. compile wakfu.exe (CMake + ninja + Clang/LLD).
// Sans argument : fenêtre du lanceur (lanceur.cpp) ; avec des arguments : ligne de commande ci-dessous (tests).
#define SDL_MAIN_HANDLED
#include <windows.h>
#include <shellapi.h>
#include <zstd.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "gen_imports.h"
#include "installer.h"

namespace wk {

Suivi* g_suivi = nullptr;
std::string u8(const fs::path& p) { auto s = p.generic_u8string(); return std::string(s.begin(), s.end()); }
fs::path fromU8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

namespace {

using wk::u8;
std::string u8(const std::wstring& w) { return wk::u8(fs::path(w)); }
HANDLE g_job = nullptr;   // fenêtre : outils lancés dans un « job », arrêtés si le lanceur se ferme

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
// Fenêtre : sortie lue par un tube et envoyée au suivi (journal, progression), sans fenêtre de console,
// dans le job du lanceur. jeu = wakfu.exe lancé depuis la fenêtre : ni tube ni job (sa propre console).
int runProcess(const std::vector<std::wstring>& args, const fs::path& cwd = {}, bool jeu = false)
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
    else if (!g_suivi && !GetConsoleWindow())
    {
        // Ligne de commande sans console (sorties redirigées : tests) : l'exécutable est une application à fenêtre,
        // ses descripteurs standard sont donc passés explicitement, et les outils ne doivent pas ouvrir de console.
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        flags = CREATE_NO_WINDOW;
    }
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

int step(const char* what, const std::vector<std::wstring>& args, const fs::path& cwd = {}, int accepte = 0)
{
    int rc = runProcess(args, cwd);
    if (rc != 0 && rc != accepte) throw Error(std::string(what) + " : échec (code " + std::to_string(rc) + ")");
    return rc;
}

std::wstring w(const fs::path& p) { return p.wstring(); }
std::wstring wdef(const char* name, const fs::path& p) { return L"-D" + fs::path(name).wstring() + L"=" + fromU8(u8(p)).wstring(); }
void etape(Etape e) { if (g_suivi) g_suivi->etape(e); }

// ---- Console ----
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

bool isInside(const fs::path& p, const fs::path& dir)
{
    auto r = fs::weakly_canonical(p).lexically_relative(fs::weakly_canonical(dir));
    return !r.empty() && *r.begin() != "..";
}

// p est dir ou dans dir, casse ignorée (chemins Windows)
bool sousDossier(const fs::path& p, const fs::path& dir)
{
    std::error_code ec;
    std::wstring a = fs::weakly_canonical(p, ec).wstring(), b = fs::weakly_canonical(dir, ec).wstring();
    if (a.empty() || b.empty()) return false;
    for (auto* x : { &a, &b })
    {
        std::replace(x->begin(), x->end(), L'/', L'\\');
        CharLowerBuffW(x->data(), DWORD(x->size()));
        while (x->size() > 1 && x->back() == L'\\') x->pop_back();
    }
    return a.compare(0, b.size(), b) == 0 && (a.size() == b.size() || a[b.size()] == L'\\');
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
    say("WakfuRecomp : construit Islands of Wakfu pour Windows à partir de VOTRE copie du jeu (Xbox 360).\n\n"
        "usage : WakfuRecomp.exe [--dossier <D>] [--sans-lancer] <jeu> [<mise à jour>]\n"
        "  <jeu>, <mise à jour> : paquet XBLA (STFS) du jeu et de la Title Update 2, image ISO/XISO, dossier extrait,\n"
        "                         ou un dossier qui contient ces fichiers. Glisser-déposer sur l'exécutable fonctionne.\n"
        "  --dossier <D>  : dossier de travail (défaut %%LOCALAPPDATA%%\\WakfuRecomp ; prévoir ~3 Go libres)\n"
        "Mode portable : un fichier portable.txt à côté de WakfuRecomp.exe -> dossier de travail et données\n"
        "  (sauvegardes, réglages) dans <dossier de l'exe>\\WakfuRecomp.\n"
        "  --sans-lancer  : ne pas lancer le jeu à la fin\n"
        "Sans argument : fenêtre du lanceur (choix du jeu, recompilation, réglages, jeu).\n");
    return 2;
}

} // namespace

fs::path selfPath()
{
    wchar_t selfBuf[32768];
    DWORD n = GetModuleFileNameW(nullptr, selfBuf, 32768);
    if (!n || n >= 32768) throw Error("chemin de l'exécutable introuvable");
    return fs::path(selfBuf);
}

// Mode portable : un fichier portable.txt à côté de WakfuRecomp.exe ; dossier de travail et données à côté de l'exe.
// Relu à chaque appel : la fenêtre crée portable.txt quand le joueur choisit ce mode.
bool portableMode()
{
    std::error_code ec;
    return fs::exists(selfPath().parent_path() / "portable.txt", ec);
}

fs::path appDataDir()
{
    fs::path a = envPath(L"APPDATA");
    return a.empty() ? a : a / "WakfuRecomp";
}

RefusPortable verifierPortable(double* gb)
{
    *gb = -1;
    const fs::path dir = selfPath().parent_path();
    for (const wchar_t* v : { L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramW6432" })
        if (fs::path p = envPath(v); !p.empty() && sousDossier(dir, p)) return RefusPortable::ProgramFiles;
    for (const wchar_t* v : { L"OneDrive", L"OneDriveConsumer", L"OneDriveCommercial" })
        if (fs::path p = envPath(v); !p.empty() && sousDossier(dir, p)) return RefusPortable::OneDrive;
    // Fichier test (supprimé à la fermeture), nom propre à ce processus : rien d'existant n'est touché
    fs::path t = dir / (".wakfurecomp-test-" + std::to_string(GetCurrentProcessId()) + ".tmp");
    HANDLE h = CreateFileW(t.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return RefusPortable::Ecriture;
    DWORD n = 0;
    bool ok = WriteFile(h, "x", 1, &n, nullptr) && n == 1;
    CloseHandle(h);
    if (!ok) return RefusPortable::Ecriture;
    *gb = freeGb(dir);
    return *gb >= 0 && *gb < 3 ? RefusPortable::Espace : RefusPortable::Aucun;
}

void activerPortable()
{
    fs::path p = selfPath().parent_path() / "portable.txt";
    std::ofstream o(p, std::ios::binary | std::ios::app);   // jamais tronqué : un portable.txt existant est gardé
    if (!o || !o.flush() || !portableMode()) throw Error("création impossible : " + u8(p));
}

fs::path defaultWorkDir()
{
    if (portableMode()) return selfPath().parent_path() / "WakfuRecomp";
    fs::path la = envPath(L"LOCALAPPDATA");
    if (la.empty()) throw Error("LOCALAPPDATA non défini : utiliser --dossier <D>");
    return la / "WakfuRecomp";
}

void openWorkDir(Install& in, fs::path D)
{
    D = fs::absolute(D);
    fs::create_directories(D);
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

// Données du joueur (sauvegardes, réglages, config.toml) : %APPDATA%\WakfuRecomp (itinérant), sinon (ou en mode
// portable) <D>\userdata.
// Première fois : l'ancien <D>\userdata est copié (jamais supprimé), pour ne perdre aucune sauvegarde.
// Seul point de calcul du dossier des données (ligne de commande, fenêtre, jouer.cmd).
fs::path dataDir(const fs::path& D)
{
    fs::path a = portableMode() ? fs::path() : envPath(L"APPDATA");
    if (a.empty()) { fs::create_directories(D / "userdata"); return D / "userdata"; }
    fs::path target = a / "WakfuRecomp";
    std::error_code ec;
    if (!fs::exists(target) && fs::is_directory(D / "userdata") && !fs::is_empty(D / "userdata", ec))
    {
        fs::copy(D / "userdata", target, fs::copy_options::recursive, ec);
        if (ec) throw Error("copie de " + u8(D / "userdata") + " vers " + u8(target) + " : " + ec.message());
        say("Données copiées vers %s (l'ancien dossier est conservé)\n", u8(target).c_str());
    }
    fs::create_directories(target);
    return target;
}

bool isPrepared(const Install& in) { return fs::exists(in.G / "patched" / "image.bin"); }
bool isInstalled(const Install& in) { return isPrepared(in) && fs::exists(in.D / "wakfu.exe"); }
bool isUpToDate(const Install& in)
{
    std::string id;
    std::ifstream(in.W / "out" / ".recomp-ok") >> id;
    return id == in.id;
}
double freeGb(const fs::path& D)
{
    ULARGE_INTEGER freeB{};
    return GetDiskFreeSpaceExW(D.c_str(), &freeB, nullptr, nullptr) ? freeB.QuadPart / double(1 << 30) : -1;
}
void forceRebuild(const Install& in)
{
    fs::remove(in.W / "out" / ".recomp-ok");
    fs::remove_all(in.W / "build");
}

int launchGame(const fs::path& D)
{
    fs::path g = D / "work" / "game";
    const fs::path data = dataDir(D);
    say("Lancement : %s (données : %s)\n", u8(D / "wakfu.exe").c_str(), u8(data).c_str());
    return runProcess({ w(D / "wakfu.exe"), w(g / "patched" / "image.bin"), w(g / "patched" / "default.xex"),
                        w(g / "base"), w(g / "tu2"), w(data) }, D, true);
}

// 1. Outils
void ensureTools(Install& in)
{
    etape(EtOutils);
    const fs::path& D = in.D, &T = in.T;
    if (!fs::exists(T / ".complet"))
    {
        say("[1/5] Extraction des outils dans %s\n", u8(T).c_str());
        fs::path tmp = D / ("outils-" + in.id + ".tmp");
        fs::remove_all(tmp);
        fs::remove_all(T);
        extractPayload(selfPath(), Trailer{ in.off, in.size, in.id }, tmp);
        std::ofstream(tmp / ".complet").close();
        fs::rename(tmp, T);
        for (auto& e : fs::directory_iterator(D))   // anciennes versions des outils (marquées par nous)
            if (e.is_directory() && e.path() != T && u8(e.path().filename()).rfind("outils-", 0) == 0 && fs::exists(e.path() / ".complet"))
                fs::remove_all(e.path());
    }
    else say("[1/5] Outils déjà extraits : %s\n", u8(T).c_str());
    std::wstring path = w(T / "llvm-mingw" / "bin") + L";" + w(T / "bin") + L";" + w(T / "cmake" / "bin") + L";" + envPath(L"PATH").wstring();
    SetEnvironmentVariableW(L"PATH", path.c_str());
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
        fs::remove_all(G);
        fs::remove(in.W / "out" / ".recomp-ok");
        std::vector<std::wstring> a = { w(in.bin / "wakfu-prepare.exe"), L"-o", w(G) };
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
    const fs::path &D = in.D, &W = in.W, &G = in.G, &T = in.T, &bin = in.bin;
    // 3. C++ recompilé (XenonRecomp écrit dans work/out/ppc d'après config/wakfu.toml)
    fs::path recompDone = W / "out" / ".recomp-ok";
    etape(EtXenon);
    if (!isUpToDate(in))   // jamais fait, jeu re-préparé ou autre version des outils
    {
        say("[3/5] Recompilation du code PowerPC (XenonRecomp)\n");
        fs::remove_all(D / "config");
        fs::copy(T / "src" / "config", D / "config", fs::copy_options::recursive);
        fs::remove_all(W / "out" / "ppc");
        fs::create_directories(W / "out" / "ppc");
        step("XenonRecomp", { w(bin / "XenonRecomp.exe"), w(D / "config" / "wakfu.toml"), w(T / "sdk" / "XenonUtils" / "ppc_context.h") }, D / "config");
        etape(EtImports);
        say("  %s\n", genImports(G / "patched" / "default.xex", G / "patched" / "patched.txt", T / "sdk" / "XenonUtils" / "xbox", W / "gen").c_str());
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
        if (id != in.id) fs::remove_all(build);   // autre version des outils : configuration à refaire
    }
    if (!fs::exists(build / ".outils"))   // écrit seulement après une configuration réussie
    {
        fs::remove_all(build);
        say("[4/5] Configuration (CMake)\n");
        fs::path L = T / "llvm-mingw" / "bin";
        step("configuration CMake", { w(T / "cmake" / "bin" / "cmake.exe"), L"-S", w(T / "src" / "runtime"), L"-B", w(build), L"-G", L"Ninja",
            wdef("CMAKE_MAKE_PROGRAM", bin / "ninja.exe"), L"-DCMAKE_BUILD_TYPE=Release",
            wdef("CMAKE_C_COMPILER", L / "clang.exe"), wdef("CMAKE_CXX_COMPILER", L / "clang++.exe"),
            wdef("PPC_DIR", W / "out" / "ppc"), wdef("SIMDE_DIR", T / "sdk" / "thirdparty" / "simde"), wdef("GEN_DIR", W / "gen"),
            wdef("glslang_DIR", T / "deps" / "glslang" / "lib" / "cmake" / "glslang"),
            wdef("SDL2_DIR", T / "deps" / "SDL2" / "lib" / "cmake" / "SDL2"),
            wdef("FFMPEG_PREFIX", T / "deps" / "ffmpeg"), wdef("VULKAN_SYS_INCLUDE", T / "deps" / "vulkan" / "include") }, W);
        std::ofstream(build / ".outils") << in.id;
    }
    etape(EtCompilation);
    say("[4/5] Compilation de wakfu.exe (long : plusieurs minutes à une heure selon le processeur)\n");
    step("compilation", { w(T / "cmake" / "bin" / "cmake.exe"), L"--build", w(build), L"--target", L"wakfu" }, W);

    // 5. Installation
    etape(EtInstallation);
    fs::copy_file(build / "wakfu.exe", D / "wakfu.exe", fs::copy_options::overwrite_existing);
    std::ofstream(D / "jouer.cmd", std::ios::binary)
        << "@echo off\r\ncd /d \"%~dp0\"\r\n\"%~dp0wakfu.exe\" \"%~dp0work\\game\\patched\\image.bin\" \"%~dp0work\\game\\patched\\default.xex\" "
           "\"%~dp0work\\game\\base\" \"%~dp0work\\game\\tu2\" \"" << (dataDir(D) == D / "userdata" ? "%~dp0userdata" : "%APPDATA%\\WakfuRecomp") << "\"\r\n";
    say("[5/5] Prêt : %s\n  Pour jouer : relancer WakfuRecomp.exe sans argument, ou %s\n", u8(D / "wakfu.exe").c_str(), u8(D / "jouer.cmd").c_str());
}

void createJob()   // fenêtre : les outils (et leurs enfants : ninja, clang) s'arrêtent avec le lanceur
{
    g_job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (g_job) SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof li);
}

namespace {

int realMain(int argc, wchar_t** argv)
{
    fs::path D;
    bool launch = true;
    std::vector<fs::path> inputs;
    for (int i = 1; i < argc; ++i)
    {
        std::wstring a = argv[i];
        if (a == L"--dossier" && i + 1 < argc) D = argv[++i];
        else if (a == L"--sans-lancer") launch = false;
        else if (a == L"-h" || a == L"--help" || a == L"/?" || a == L"--dossier") return usage();
        else inputs.emplace_back(a);
    }
    Install in;
    openWorkDir(in, D.empty() ? defaultWorkDir() : D);
    D = in.D;
    bool prepared = isPrepared(in);

    if (inputs.empty() && prepared && fs::exists(D / "wakfu.exe")) return launch ? launchGame(D) : 0;
    if (inputs.empty() && !prepared)
    {
        usage();
        say("\nChemin(s) du jeu et de la mise à jour (glisser les fichiers dans cette fenêtre), puis Entrée :\n> ");
        inputs = splitPaths(readLine());
        if (inputs.empty()) return 2;
    }
    verifierEntrees(inputs);
    for (auto& i : inputs)
        if (isInside(i, D)) throw Error("l'entrée " + u8(i) + " est dans le dossier de travail " + u8(D) + " : la déplacer");

    double gb = freeGb(D);
    if (gb >= 0 && gb < 3)
        say("ATTENTION : %.1f Go libres sur le disque de %s (~3 Go nécessaires).\n", gb, u8(D).c_str());

    ensureTools(in);
    prepareGame(in, inputs);
    buildGame(in);
    return launch ? launchGame(D) : 0;
}

} // namespace
} // namespace wk

int wmain(int argc, wchar_t** argv)
{
    if (argc <= 1)   // double-clic : fenêtre du lanceur
    {
        wk::createJob();
        return wk::guiMain();
    }
    wk::openConsole();
    SetConsoleOutputCP(CP_UTF8);
    int rc;
    try { rc = wk::realMain(argc, argv); }
    catch (const std::exception& e)
    {
        wk::say("\nERREUR : %s\n", e.what());
        rc = 1;
    }
    if (wk::g_ownConsole && rc != 0)
    {
        wk::say("\nAppuyer sur Entrée pour fermer.");
        wk::readLine();
    }
    return rc;
}
