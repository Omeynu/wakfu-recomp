// Fenêtre du lanceur (WakfuRecomp.exe sans argument) : Dear ImGui sur SDL2 avec le rendu 2D de SDL (SDL_Renderer).
// Premier lancement : accueil -> choix du jeu et de la mise à jour -> vérification -> recompilation -> réglages -> Jouer.
// Ensuite : écran Jouer (réglages, recompiler). Les étapes sont celles de la ligne de commande (installer.h),
// exécutées dans un fil de travail ; leur sortie va dans <dossier de travail>\journal-lanceur.txt.
// Illustrations (KeyArt.png, Background.png) : lues dans la copie du jeu du joueur, jamais distribuées.
#define SDL_MAIN_HANDLED
#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <wincodec.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#include <zlib.h>
#endif
#include <SDL.h>
#include <SDL_syswm.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <thread>
#include "../../runtime/src/config.h"   // pas de -I runtime/src : son process.h masquerait celui du système
#include "installer.h"
#include "../../runtime/src/png.h"
#include "textes.h"

namespace wk {

namespace {

namespace cfg = wk::config;
using Clock = std::chrono::steady_clock;

// ---- Thème : tons d'automne d'Islands of Wakfu ----
ImVec4 rgb(uint32_t c, float a = 1.0f) { return ImVec4(((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, a); }
ImU32 u32(uint32_t c, float a = 1.0f) { return ImGui::ColorConvertFloat4ToU32(rgb(c, a)); }
constexpr uint32_t kFondHaut = 0x3A2416, kFondBas = 0x120B07, kCarte = 0x24170F, kBord = 0x6B4527, kOrange = 0xE8892B,
                   kOrangeClair = 0xF4A04A, kOrangeFonce = 0xC66D1C, kSecondaire = 0x3E2a1b, kSecondaireClair = 0x543a25,
                   kTexte = 0xF7EBDD, kTexteDoux = 0xC9AF94, kVert = 0x9BCB6E, kRouge = 0xEE6A50, kJaune = 0xF0C15A,
                   kChamp = 0x1A100A;

float g_s = 1.0f;                // échelle de l'affichage (DPI)
float S(float v) { return v * g_s; }
ImFont *g_regular = nullptr, *g_semibold = nullptr;
const float kTexteTaille = 17.0f;

void police(ImFont* f, float taille) { ImGui::PushFont(f, taille); }
void finPolice() { ImGui::PopFont(); }

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...)
{
    char b[2048];
    va_list a;
    va_start(a, f);
    std::vsnprintf(b, sizeof b, f, a);
    va_end(a);
    return b;
}

// ---- Journal et suivi du travail ----
struct Travail
{
    std::mutex m;
    bool enCours = false, fini = false, echec = false, mauvaiseCopie = false;
    Etape etape = EtOutils;
    double frac = -1;               // fraction de l'étape, < 0 : inconnue
    Clock::time_point debutEtape;
    std::string erreur, detail;     // message d'erreur, fichier en cours de compilation
    std::string sha;                // SHA-1 de l'image donné par wakfu-prepare (affichage seulement)
    std::string avantJournal;       // lignes écrites avant l'ouverture du journal (choix du mode)
    std::deque<std::string> fin;    // dernières lignes de sortie (affichées en cas d'échec)
    FILE* journal = nullptr;
};
Travail g_t;

class SuiviFenetre : public Suivi
{
public:
    void etape(Etape e) override
    {
        std::lock_guard l(g_t.m);
        g_t.etape = e, g_t.frac = -1, g_t.detail.clear(), g_t.debutEtape = Clock::now();
    }
    void avance(double f) override { std::lock_guard l(g_t.m); g_t.frac = f; }
    void ligne(const std::string& s) override
    {
        std::lock_guard l(g_t.m);
        if (g_t.journal) { std::fputs(s.c_str(), g_t.journal); if (s.empty() || s.back() != '\n') std::fputc('\n', g_t.journal); std::fflush(g_t.journal); }
        else if (g_t.avantJournal.size() < 65536) g_t.avantJournal += s + (s.empty() || s.back() != '\n' ? "\n" : "");
        std::string t = s;
        while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
        if (t.empty()) return;
        g_t.fin.push_back(t);
        if (g_t.fin.size() > 12) g_t.fin.pop_front();
        // ninja : « [n/N] Building CXX object ... »
        unsigned n, N;
        if (t[0] == '[' && std::sscanf(t.c_str(), "[%u/%u]", &n, &N) == 2 && N > 0 && g_t.etape == EtCompilation)
        {
            g_t.frac = double(n) / N;
            size_t sl = t.find_last_of("/\\");
            g_t.detail = sl == std::string::npos ? std::string() : t.substr(sl + 1);
        }
        // XenonRecomp : « Recompiling functions... 13.4% »
        float pc;
        if (std::sscanf(t.c_str(), "Recompiling functions... %f%%", &pc) == 1) g_t.frac = pc / 100.0;
        // wakfu-prepare : « TU appliquée, image : SHA-1 <40 chiffres> », affiché si la version est refusée (code de sortie 3)
        char sha[41];
        if (std::sscanf(t.c_str(), "TU appliqu\xC3\xA9" "e, image : SHA-1 %40[0-9a-f]", sha) == 1) g_t.sha = sha;
    }
};
SuiviFenetre g_suiviFenetre;

void journal(const std::string& s) { g_suiviFenetre.ligne(s); }

FILE* ouvrirJournal(const fs::path& p)
{
#ifdef _WIN32
    return _wfopen(p.c_str(), L"ab");
#else
    return std::fopen(p.c_str(), "ab");
#endif
}

// ---- Ressources : police Inter (Windows : wakfu-installer.rc ; Linux : #embed), images du jeu (WIC ; Linux : zlib) ----
#ifdef _WIN32
ImFont* policeRessource(const wchar_t* nom)
{
    HRSRC r = FindResourceW(nullptr, nom, MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL h = r ? LoadResource(nullptr, r) : nullptr;
    void* p = h ? LockResource(h) : nullptr;
    if (!p) throw Error("police absente de l'exécutable");
    ImFontConfig c;
    c.FontDataOwnedByAtlas = false;   // données de la ressource : ni copiées ni libérées
    c.OversampleH = 2;
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(p, int(SizeofResource(nullptr, r)), kTexteTaille, &c);
}
#else
const unsigned char kInterRegular[] = {
#embed "Inter-Regular.ttf"
};
const unsigned char kInterSemiBold[] = {
#embed "Inter-SemiBold.ttf"
};
ImFont* policeRessource(const unsigned char* p, size_t n)
{
    ImFontConfig c;
    c.FontDataOwnedByAtlas = false;   // données du programme : ni copiées ni libérées
    c.OversampleH = 2;
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(p), int(n), kTexteTaille, &c);
}
#endif

struct Image { SDL_Texture* tex = nullptr; int w = 0, h = 0; };

#ifndef _WIN32
// PNG 8 bits RVB ou RVBA, non entrelacé (images du jeu) -> RVBA ; vide si autre format ou fichier endommagé
std::vector<uint8_t> lirePng(const fs::path& p, uint32_t& w, uint32_t& h)
{
    std::ifstream f(p, std::ios::binary);
    std::vector<uint8_t> b{ std::istreambuf_iterator<char>(f), {} }, z, px;
    auto be = [&](size_t o) { return uint32_t(b[o]) << 24 | uint32_t(b[o + 1]) << 16 | uint32_t(b[o + 2]) << 8 | b[o + 3]; };
    if (b.size() < 33 || std::memcmp(b.data(), "\x89PNG\r\n\x1a\n", 8) != 0 || std::memcmp(&b[12], "IHDR", 4) != 0) return {};
    w = be(16), h = be(20);
    const int type = b[25];
    if (b[24] != 8 || (type != 2 && type != 6) || b[28] != 0 || w == 0 || h == 0 || w > 8192 || h > 8192) return {};
    for (size_t o = 8; o + 12 <= b.size();)
    {
        uint32_t n = be(o);
        if (n > b.size() - o - 12) return {};
        if (std::memcmp(&b[o + 4], "IDAT", 4) == 0) z.insert(z.end(), b.begin() + o + 8, b.begin() + o + 8 + n);
        o += 12 + size_t(n);
    }
    const size_t bpp = type == 6 ? 4 : 3, ligne = size_t(w) * bpp;
    std::vector<uint8_t> raw((ligne + 1) * h);
    uLongf lg = uLongf(raw.size());
    if (uncompress(raw.data(), &lg, z.data(), uLong(z.size())) != Z_OK || lg != raw.size()) return {};
    std::vector<uint8_t> prec(ligne, 0), cur(ligne);
    px.resize(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
    {
        const uint8_t* s = &raw[y * (ligne + 1)];
        for (size_t i = 0; i < ligne; ++i)
        {
            int a = i >= bpp ? cur[i - bpp] : 0, u = prec[i], c = i >= bpp ? prec[i - bpp] : 0, v = s[1 + i];
            switch (s[0])
            {
            case 0: break;
            case 1: v += a; break;
            case 2: v += u; break;
            case 3: v += (a + u) / 2; break;
            case 4: { int q = a + u - c, pa = std::abs(q - a), pb = std::abs(q - u), pc = std::abs(q - c);
                      v += pa <= pb && pa <= pc ? a : pb <= pc ? u : c; break; }
            default: return {};
            }
            cur[i] = uint8_t(v);
        }
        for (uint32_t x = 0; x < w; ++x)
            for (int k = 0; k < 4; ++k) px[(size_t(y) * w + x) * 4 + k] = k < int(bpp) ? cur[x * bpp + k] : 255;
        std::swap(prec, cur);
    }
    return px;
}

Image chargerImage(SDL_Renderer* ren, const fs::path& p)
{
    Image im;
    std::error_code ec;
    if (!fs::exists(p, ec)) return im;
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> px = lirePng(p, w, h);
    if (!px.empty()) im.tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, int(w), int(h));
    if (im.tex) { SDL_UpdateTexture(im.tex, nullptr, px.data(), int(w * 4)); SDL_SetTextureBlendMode(im.tex, SDL_BLENDMODE_BLEND); im.w = int(w), im.h = int(h); }
    else journal("image illisible : " + u8(p));
    return im;
}
#else
Image chargerImage(SDL_Renderer* ren, const fs::path& p)
{
    Image im;
    std::error_code ec;
    if (!fs::exists(p, ec)) return im;
    IWICImagingFactory* f = nullptr;
    IWICBitmapDecoder* d = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICBitmapSource* rgba = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    if (SUCCEEDED(hr)) hr = f->CreateDecoderFromFilename(p.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &d);
    if (SUCCEEDED(hr)) hr = d->GetFrame(0, &fr);
    if (SUCCEEDED(hr)) hr = WICConvertBitmapSource(GUID_WICPixelFormat32bppRGBA, fr, &rgba);
    UINT w = 0, h = 0;
    if (SUCCEEDED(hr)) hr = rgba->GetSize(&w, &h);
    if (SUCCEEDED(hr) && w && h && w <= 8192 && h <= 8192)
    {
        std::vector<uint8_t> px(size_t(w) * h * 4);
        hr = rgba->CopyPixels(nullptr, w * 4, UINT(px.size()), px.data());
        if (SUCCEEDED(hr))
        {
            im.tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, int(w), int(h));
            if (im.tex) { SDL_UpdateTexture(im.tex, nullptr, px.data(), int(w * 4)); SDL_SetTextureBlendMode(im.tex, SDL_BLENDMODE_BLEND); }
            im.w = int(w), im.h = int(h);
        }
    }
    if (!im.tex) journal(fmt("image illisible (%08lx) : %s", (unsigned long)hr, u8(p).c_str()));
    for (IUnknown* u : { (IUnknown*)rgba, (IUnknown*)fr, (IUnknown*)d, (IUnknown*)f })
        if (u) u->Release();
    return im;
}
#endif

// ---- Petits éléments d'interface ----
bool bouton(const char* label, bool primaire, ImVec2 taille = ImVec2(0, 0), bool actif = true)
{
    ImGui::PushStyleColor(ImGuiCol_Button, rgb(primaire ? kOrange : kSecondaire));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rgb(primaire ? kOrangeClair : kSecondaireClair));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, rgb(primaire ? kOrangeFonce : kSecondaire));
    ImGui::PushStyleColor(ImGuiCol_Text, rgb(primaire ? 0x2A1406 : kTexte));
    police(g_semibold, kTexteTaille);
    if (!actif) ImGui::BeginDisabled();
    bool r = ImGui::Button(label, taille);
    if (!actif) ImGui::EndDisabled();
    finPolice();
    ImGui::PopStyleColor(4);
    return r;
}

float largeurBouton(const char* label)
{
    police(g_semibold, kTexteTaille);
    float w = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2;
    finPolice();
    return w;
}

void titre(const char* t, float taille = 30.0f)
{
    police(g_semibold, taille);
    ImGui::TextUnformatted(t);
    finPolice();
}

void texteDoux(const char* t, float taille = 15.0f)
{
    police(g_regular, taille);
    ImGui::PushStyleColor(ImGuiCol_Text, rgb(kTexteDoux));
    ImGui::TextWrapped("%s", t);
    ImGui::PopStyleColor();
    finPolice();
}

void texteCouleur(uint32_t c, const char* t, ImFont* f = nullptr, float taille = kTexteTaille)
{
    police(f ? f : g_regular, taille);
    ImGui::PushStyleColor(ImGuiCol_Text, rgb(c));
    ImGui::TextWrapped("%s", t);
    ImGui::PopStyleColor();
    finPolice();
}

enum class Icone { Ok, Erreur, Attente, EnCours, Attention, Numero };

// Pastille de 24 px alignée sur la ligne de texte qui suit (SameLine)
void icone(Icone i, int numero = 0)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = S(11);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float dy = (ImGui::GetTextLineHeight() - 2 * r) * 0.5f;
    ImVec2 c(p.x + r, p.y + r + dy);
    switch (i)
    {
    case Icone::Ok:
        dl->AddCircleFilled(c, r, u32(kVert));
        dl->AddPolyline(std::vector<ImVec2>{ { c.x - r * 0.45f, c.y + r * 0.02f }, { c.x - r * 0.1f, c.y + r * 0.38f }, { c.x + r * 0.48f, c.y - r * 0.34f } }.data(),
                        3, u32(0x1E2A14), 0, S(2.4f));
        break;
    case Icone::Erreur:
        dl->AddCircleFilled(c, r, u32(kRouge));
        dl->AddLine({ c.x - r * 0.38f, c.y - r * 0.38f }, { c.x + r * 0.38f, c.y + r * 0.38f }, u32(0x2A0E08), S(2.4f));
        dl->AddLine({ c.x + r * 0.38f, c.y - r * 0.38f }, { c.x - r * 0.38f, c.y + r * 0.38f }, u32(0x2A0E08), S(2.4f));
        break;
    case Icone::Attention:
        dl->AddCircleFilled(c, r, u32(kJaune));
        dl->AddLine({ c.x, c.y - r * 0.5f }, { c.x, c.y + r * 0.12f }, u32(0x2A1C06), S(2.4f));
        dl->AddCircleFilled({ c.x, c.y + r * 0.45f }, S(1.6f), u32(0x2A1C06));
        break;
    case Icone::Attente:
        dl->AddCircle(c, r - S(1), u32(kTexteDoux, 0.5f), 0, S(2));
        break;
    case Icone::EnCours:
    {
        dl->AddCircle(c, r - S(1), u32(kOrange, 0.25f), 0, S(2.5f));
        float a = float(ImGui::GetTime()) * 5.0f;
        dl->PathArcTo(c, r - S(1), a, a + 2.0f, 16);
        dl->PathStroke(u32(kOrange), 0, S(2.5f));
        break;
    }
    case Icone::Numero:
    {
        dl->AddCircleFilled(c, r, u32(kOrange, 0.18f));
        dl->AddCircle(c, r, u32(kOrange), 0, S(1.5f));
        char b[4];
        std::snprintf(b, sizeof b, "%d", numero);
        police(g_semibold, 14);
        ImVec2 ts = ImGui::CalcTextSize(b);
        dl->AddText({ c.x - ts.x * 0.5f, c.y - ts.y * 0.5f }, u32(kOrangeClair), b);
        finPolice();
        break;
    }
    }
    ImGui::Dummy(ImVec2(2 * r, ImGui::GetTextLineHeight()));
    ImGui::SameLine(0, S(12));
}

// Barre de progression arrondie (fraction < 0 : animation indéterminée)
void barre(float frac, float hauteur)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = S(hauteur), r = h * 0.5f;
    dl->AddRectFilled(p, { p.x + w, p.y + h }, u32(kChamp), r);
    if (frac >= 0)
    {
        float x = std::max(h, w * std::min(frac, 1.0f));
        dl->AddRectFilled(p, { p.x + x, p.y + h }, u32(kOrange), r);
    }
    else
    {
        float t = std::fmod(float(ImGui::GetTime()) * 0.6f, 1.0f), seg = w * 0.28f;
        float x0 = p.x + (w + seg) * t - seg;
        dl->PushClipRect(p, { p.x + w, p.y + h }, true);
        dl->AddRectFilled({ std::max(x0, p.x), p.y }, { std::min(x0 + seg, p.x + w), p.y + h }, u32(kOrange), r);
        dl->PopClipRect();
    }
    ImGui::Dummy(ImVec2(w, h));
}

bool debutCarte(const char* id, ImVec2 pos, ImVec2 taille, uint32_t fond = kCarte, float alpha = 0.93f)
{
    ImGui::SetCursorPos(pos);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, rgb(fond, alpha));
    ImGui::PushStyleColor(ImGuiCol_Border, rgb(kBord, 0.55f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(40), S(34)));
    bool r = ImGui::BeginChild(id, taille, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    return r;
}
void finCarte() { ImGui::EndChild(); }

// ---- Application ----
enum class Ecran { Accueil, Choix, Verification, Progression, Reglages, Jouer, Fatale, Anciennes, Desinstaller, Desinstalle };
enum class Job { Aucun, Verification, Construction, Suppression };

struct Fichier { fs::path p; bool ok = false; std::string erreur; };

struct Commande { const char* nom; Texte texte; const char* defaut; };
const Commande kCommandes[] = {
    { "a", T_CmdA, "Return" }, { "b", T_CmdB, "Escape" }, { "x", T_CmdX, "X" }, { "y", T_CmdY, "Y" },
    { "start", T_CmdStart, "Space" }, { "back", T_CmdBack, "Backspace" }, { "lb", T_CmdLb, "PageUp" }, { "rb", T_CmdRb, "PageDown" },
    { "lt", T_CmdLt, "Home" }, { "rt", T_CmdRt, "End" }, { "haut", T_CmdHaut, "Up" }, { "bas", T_CmdBas, "Down" },
    { "gauche", T_CmdGauche, "Left" }, { "droite", T_CmdDroite, "Right" }, { "clic_gauche", T_CmdClicG, "C" }, { "clic_droit", T_CmdClicD, "V" },
    { "stick_gauche_haut", T_CmdSgH, "W" }, { "stick_gauche_bas", T_CmdSgB, "S" }, { "stick_gauche_gauche", T_CmdSgG, "A" },
    { "stick_gauche_droite", T_CmdSgD, "D" }, { "stick_droit_haut", T_CmdSdH, "I" }, { "stick_droit_bas", T_CmdSdB, "K" },
    { "stick_droit_gauche", T_CmdSdG, "J" }, { "stick_droit_droite", T_CmdSdD, "L" },
};

struct App
{
    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    Install in;
    fs::path data, journalPath;
    Ecran ecran = Ecran::Accueil;
    Job job = Job::Aucun;
    std::thread fil;
    Fichier jeu, maj;
    std::string refus;               // fichier glissé refusé
    bool dejaPrepare = false;        // vérification : copie préparée lors d'un lancement précédent
    bool verifOk = false;
    bool finRecompilation = false;   // bannière sur l'écran des réglages
    Ecran retourReglages = Ecran::Jouer;
    cfg::Config c;
    std::string erreurConfig;
    int attenteTouche = -1;          // commande en attente d'une touche (onglet Clavier)
    int ongletForce = -1;            // capture : onglet à afficher
    Image fond, keyart;
    bool imagesChargees = false;
    bool aJour = true;               // C++ généré par cette version des outils (sinon : inviter à recompiler)
    std::atomic<bool> jeuEnCours{ false };
    std::atomic<int> codeJeu{ 0 };
    std::string messageJeu;
    bool confirmerRecompilation = false, confirmerQuitter = false, quitter = false;
    bool confirmerRestauration = false;   // copies de secours des sauvegardes
    std::vector<std::string> copies;
    std::string copieChoisie, messageSecours;
    std::string fatale;
    double gb = -1;
    ImVec2 zoneMin[2], zoneMax[2];   // zones de dépôt « Jeu » et « Mise à jour » (coordonnées de la fenêtre)
    bool garderCompilation = false;  // ménage après installation sauf si coché
    bool iconeBureau = false;        // icône du bureau créée à la fin de l'installation si cochée
    std::vector<std::string> manquants;   // Linux : dépendances système manquantes (échec de la vérification)
    std::vector<Ancienne> anciennes;      // anciennes installations proposées à la suppression (une fois)
    bool anciennesRepondu = false;        // « Supprimer » ou « Pas maintenant » : marqueur .anciennes-vues écrit
    std::vector<std::pair<fs::path, uint64_t>> listeDesinst;   // ce que la désinstallation supprime (affiché avant)
    bool effacerSauvegardes = false, confirmerSauvegardes = false;

    // ---- Travail en arrière-plan ----
    void lancer(Job j, std::function<void()> f)
    {
        if (fil.joinable()) fil.join();
        {
            std::lock_guard l(g_t.m);
            g_t.enCours = true, g_t.fini = g_t.echec = g_t.mauvaiseCopie = false;
            g_t.sha.clear();
            g_t.erreur.clear(), g_t.fin.clear(), g_t.frac = -1, g_t.etape = EtOutils, g_t.debutEtape = Clock::now();
        }
        job = j;
        fil = std::thread([f] {
            std::string err;
            try { f(); }
            catch (const std::exception& e) { err = e.what(); }
            catch (...) { err = "erreur inconnue"; }
            if (!err.empty()) journal("ERREUR : " + err);
            std::lock_guard l(g_t.m);
            g_t.enCours = false, g_t.fini = true, g_t.echec = !err.empty(), g_t.erreur = err;
        });
    }

    void verifier(bool installerDeps = false)
    {
        ecran = Ecran::Verification;
        verifOk = false, dejaPrepare = false;
        gb = freeGb(in.D);
        std::vector<fs::path> entrees = { jeu.p, maj.p };
        lancer(Job::Verification, [this, entrees, installerDeps] {
            if (installerDeps) installerDependances();
            ensureTools(in);
            if (!prepareGame(in, entrees))   // wakfu-prepare, code 3 : SHA-1 de l'image non reconnu
            {
                supprimerDans(in.D, in.G);   // copie refusée : le choix des fichiers est à refaire
                { std::lock_guard l(g_t.m); g_t.mauvaiseCopie = true; }
                throw Error("SHA-1 de l'image (jeu + mise à jour) différent de celui attendu par config/wakfu.toml");
            }
        });
    }

    void construire(bool force)
    {
        ecran = Ecran::Progression;
        lancer(Job::Construction, [this, force] {
            if (force) forceRebuild(in);
            ensureTools(in);
            prepareGame(in, {});
            buildGame(in);
            if (!garderCompilation) menage(in);
            else journal("ménage : fichiers de compilation gardés (case cochée)");
        });
    }

    void finTravail()   // appelé par la boucle quand le fil a fini
    {
        if (fil.joinable()) fil.join();
        bool echec, mauvaise;
        std::string sha;
        { std::lock_guard l(g_t.m); echec = g_t.echec; mauvaise = g_t.mauvaiseCopie; sha = g_t.sha; g_t.fini = false; }
        Job j = job;
        job = Job::Aucun;
        if (j == Job::Verification && echec && mauvaise)   // mauvaise version : retour aux champs, cause sur le jeu
        {
            jeu.ok = false;
            jeu.erreur = tr(T_ErrVersionImage);
            refus = std::string(tr(T_MauvaiseCopieDetail)) + "\n" + fmt(tr(T_ShaImage), sha.empty() ? "?" : sha.c_str());
            ecran = Ecran::Choix;
        }
        if (j == Job::Verification && !echec) { verifOk = true; chargerImages(); construire(false); }   // « Recompiler »
        if (j == Job::Verification && echec && !mauvaise) manquants = dependancesManquantes();
        if (j == Job::Suppression && ecran == Ecran::Anciennes && !echec) finAnciennes();
        if (j == Job::Suppression && ecran == Ecran::Desinstaller && !echec) ecran = Ecran::Desinstalle;
        if (j == Job::Construction && !echec && iconeBureau)
            try { creerRaccourci(raccourciBureau(), in); journal("icône du bureau : " + u8(raccourciBureau())); }
            catch (const std::exception& e) { journal(std::string("ERREUR : ") + e.what()); }
        if (j == Job::Construction && !echec)
        {
            chargerReglages();
            aJour = true;
            finRecompilation = true;
            retourReglages = Ecran::Jouer;
            ecran = Ecran::Reglages;
        }
    }

    void chargerImages()
    {
        if (imagesChargees) return;
        imagesChargees = true;
        fs::path base = in.G / "base";
        if (const char* e = std::getenv("WAKFURECOMP_CAPTURE_IMAGES")) base = fromU8(e);
        fond = chargerImage(ren, base / "Background.png");
        keyart = chargerImage(ren, base / "KeyArt.png");
    }

    bool garderCopie = false;
    void chargerReglages()
    {
        c = cfg::Config();
        garderCopie = false;
        std::ifstream f(data / "config.toml", std::ios::binary);
        if (!f) return;
        std::stringstream ss;
        ss << f.rdbuf();
        std::vector<std::string> err;
        c = cfg::Parse(ss.str(), &err);
        for (auto& e : err) journal("[config] " + e);
        garderCopie = !err.empty();   // fichier invalide : copié en .bak avant d'être réécrit aux valeurs lues
    }

    bool enregistrerReglages()
    {
        erreurConfig.clear();
        fs::path p = data / "config.toml", tmp = data / "config.toml.tmp";
        std::error_code ec;
        if (garderCopie)
        {
            fs::copy_file(p, data / "config.toml.bak", fs::copy_options::overwrite_existing, ec);
            if (ec)
            {
                erreurConfig = fmt(tr(T_ErrEcritureConfig), (u8(data / "config.toml.bak") + " (" + ec.message() + ")").c_str());
                journal(erreurConfig);
                return false;
            }
            journal("config.toml invalide copié dans config.toml.bak");
            garderCopie = false;
        }
        {
            std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
            o << cfg::Serialize(c);
            if (!o.flush()) ec = std::make_error_code(std::errc::io_error);
        }
        if (!ec) fs::rename(tmp, p, ec);
        if (ec)
        {
            erreurConfig = fmt(tr(T_ErrEcritureConfig), (u8(p) + " (" + ec.message() + ")").c_str());
            journal(erreurConfig);
            return false;
        }
        journal("réglages enregistrés : " + u8(p));
        return true;
    }

    void jouer()
    {
        if (in.D.empty() || data.empty()) return;   // dossier pas encore ouvert : rien à écrire (chemins relatifs sinon)
        if (!enregistrerReglages()) return;
        messageJeu.clear();
        jeuEnCours = true;
        SDL_HideWindow(win);
        if (fil.joinable()) fil.join();
        fil = std::thread([this] {
            int rc;
            try { rc = launchGame(in); }
            catch (const std::exception& e) { journal(std::string("ERREUR : ") + e.what()); rc = -1; }
            journal(fmt("wakfu.exe : code de sortie %d", rc));
            codeJeu = rc;
            jeuEnCours = false;
        });
    }

    void finJeu()
    {
        if (fil.joinable()) fil.join();
        SDL_ShowWindow(win);
        SDL_RaiseWindow(win);
        chargerReglages();   // position et taille de la fenêtre écrites par le jeu à sa fermeture
        int rc = codeJeu;
        // Codes de sortie de wakfu.exe : 0 normal, 2 usage, 3 fichiers du jeu invalides, 4 mémoire, 5 erreur fatale, 6 plantage
        if (rc == 0) messageJeu.clear();
        else if (rc == 3) messageJeu = tr(T_Sortie3);
        else if (rc == 4) messageJeu = tr(T_Sortie4);
        else if (rc == 5) messageJeu = tr(T_Sortie5);
        else if (rc == 6) messageJeu = tr(T_Sortie6);
        else messageJeu = fmt(tr(T_SortieCode), rc);
    }

    // voulu : champ visé (0 jeu, 1 mise à jour, -1 aucun). classer (glisser-déposer) : un jeu ou une mise à jour
    // reconnus vont dans leur champ, où qu'ils soient déposés ; avec « Parcourir », un fichier du mauvais type est
    // refusé dans le champ visé (jeu et TU inversés). Renvoie true si le fichier a rempli un champ sans erreur.
    bool choisirFichier(const fs::path& p, int voulu, bool classer = false)
    {
        InfoPaquet info;
        Paquet r = verifierPaquet(p, &info);
        journal(fmt("entrée %s : %s, résultat %d (titre %08X, « %s »)", u8(p).c_str(), info.format.c_str(), int(r), info.titre, info.nom.c_str()));
        refus.clear();
        if (r == Paquet::Dossier) return choisirDossier(p, voulu);
        if (r == Paquet::Jeu && (classer || voulu != 1)) { jeu = { p, true, {} }; return true; }
        if (r == Paquet::MiseAJour && (classer || voulu != 0)) { maj = { p, true, {} }; return true; }
        std::string m = r == Paquet::Jeu ? tr(T_ErrInverseJeu) : r == Paquet::MiseAJour ? tr(T_ErrInverseMaj)
                      : r == Paquet::NonStfs ? tr(T_ErrNonStfs) : r == Paquet::AutreJeu ? fmt(tr(T_ErrAutreJeu), info.nom.c_str(), info.titre)
                      : r == Paquet::NiJeuNiMaj ? tr(T_ErrNiJeuNiMaj) : r == Paquet::Tronque ? tr(T_ErrTronque)
                      : r == Paquet::MajNonPriseEnCharge ? fmt(tr(T_ErrMajVersion), info.nom.c_str())
                      : r == Paquet::DossierMixte ? tr(T_ErrDossierMixte) : tr(T_ErrIllisible);
        refuser(p, voulu, m);
        return false;
    }

    void refuser(const fs::path& p, int voulu, const std::string& m)
    {
        if (voulu == 0) jeu = { p, false, m };
        else if (voulu == 1) maj = { p, false, m };
        else refus = fmt(tr(T_FichierRefuse), u8(p.filename()).c_str(), m.c_str());
    }

    // Dossier quelconque (comme wakfu-prepare) : paquets et images trouvés dedans, sur 4 niveaux, classés par en-tête
    bool choisirDossier(const fs::path& d, int voulu)
    {
        int trouves = 0, examines = 0;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(d, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator() && examines < 2000; it.increment(ec))
        {
            if (it.depth() >= 4) it.disable_recursion_pending();
            std::error_code e2;
            if (!it->is_regular_file(e2) || it->file_size(e2) < 0x1000) continue;
            ++examines;
            InfoPaquet info;
            Paquet r = verifierPaquet(it->path(), &info);
            if (r == Paquet::Jeu || r == Paquet::MiseAJour || r == Paquet::Tronque || r == Paquet::MajNonPriseEnCharge)
                trouves += choisirFichier(it->path(), voulu, true) ? 1 : 0;
        }
        if (trouves == 0) refuser(d, voulu, tr(T_ErrDossier));
        return trouves > 0;
    }

    void deposer(const fs::path& p)   // glisser-déposer : champ sous le curseur (pour un refus), sinon aucun
    {
        int gx, gy, wx, wy;
        SDL_GetGlobalMouseState(&gx, &gy);
        SDL_GetWindowPosition(win, &wx, &wy);
        ImVec2 m(float(gx - wx), float(gy - wy));
        int voulu = -1;
        for (int i = 0; i < 2; ++i)
            if (m.x >= zoneMin[i].x && m.y >= zoneMin[i].y && m.x < zoneMax[i].x && m.y < zoneMax[i].y) voulu = i;
        choisirFichier(p, voulu, true);
    }

#ifdef _WIN32
    void parcourir(int voulu)
    {
        wchar_t nom[4096] = L"";
        std::wstring titre = fromU8(tr(voulu == 0 ? T_DialogueJeu : T_DialogueMaj)).wstring();
        OPENFILENAMEW o{};
        o.lStructSize = sizeof o;
        SDL_SysWMinfo wm;
        SDL_VERSION(&wm.version);
        if (SDL_GetWindowWMInfo(win, &wm)) o.hwndOwner = wm.info.win.window;
        o.lpstrFilter = L"*.*\0*.*\0";
        o.lpstrFile = nom;
        o.nMaxFile = 4096;
        o.lpstrTitle = titre.c_str();
        o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER | OFN_HIDEREADONLY;
        if (GetOpenFileNameW(&o)) choisirFichier(fs::path(nom), voulu);
    }

    void ouvrir(const fs::path& p) { ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
#else
    // Sélecteur de fichiers du bureau (zenity : GNOME et autres ; kdialog : KDE) ; sinon glisser-déposer
    static std::string sortieCommande(const std::vector<std::string>& args)
    {
        int fd[2];
        if (pipe(fd) != 0) return {};
        std::vector<char*> v;
        for (auto& a : args) v.push_back(const_cast<char*>(a.c_str()));
        v.push_back(nullptr);
        pid_t pid = fork();
        if (pid == 0) { dup2(fd[1], 1); close(fd[0]); close(fd[1]); execvp(v[0], v.data()); _exit(127); }
        close(fd[1]);
        std::string r;
        char b[4096];
        ssize_t n;
        while (pid > 0 && (n = read(fd[0], b, sizeof b)) > 0) r.append(b, size_t(n));
        close(fd[0]);
        int st = 0;
        if (pid > 0) waitpid(pid, &st, 0);
        if (pid <= 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) return {};
        while (!r.empty() && r.back() == '\n') r.pop_back();
        return r;
    }

    void parcourir(int voulu)
    {
        const std::string t = tr(voulu == 0 ? T_DialogueJeu : T_DialogueMaj);
        std::string r = sortieCommande({ "zenity", "--file-selection", "--title=" + t });
        if (r.empty()) r = sortieCommande({ "kdialog", "--getopenfilename", ".", "--title", t });
        if (!r.empty()) choisirFichier(fromU8(r), voulu);
        else refus = tr(T_PasDeSelecteur);
    }

    void ouvrir(const fs::path& p)
    {
        if (fork() == 0) { setsid(); execlp("xdg-open", "xdg-open", p.c_str(), (char*)nullptr); _exit(127); }
    }
#endif

    // ---- Dessin ----
    void dessinerFond()
    {
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        ImVec2 sz = ImGui::GetIO().DisplaySize;
        if (fond.tex)   // image du jeu, recadrée pour couvrir la fenêtre, assombrie pour la lisibilité
        {
            float k = std::max(sz.x / fond.w, sz.y / fond.h), w = fond.w * k, h = fond.h * k;
            ImVec2 p((sz.x - w) * 0.5f, (sz.y - h) * 0.5f);
            dl->AddImage(ImTextureRef(ImTextureID(intptr_t(fond.tex))), p, { p.x + w, p.y + h });
            dl->AddRectFilledMultiColor({ 0, 0 }, sz, u32(0x140A05, 0.80f), u32(0x140A05, 0.62f), u32(0x140A05, 0.90f), u32(0x140A05, 0.94f));
        }
        else   // lueur chaude en haut à droite
            dl->AddRectFilledMultiColor({ 0, 0 }, sz, u32(kFondHaut), u32(0x6A3A18), u32(kFondBas), u32(kFondBas));
    }

    void entete()
    {
        ImGui::SetCursorPos(ImVec2(S(40), S(30)));
        ImGui::BeginGroup();
        police(g_semibold, 15);
        ImGui::PushStyleColor(ImGuiCol_Text, rgb(kOrangeClair));
        ImGui::TextUnformatted(tr(T_Marque));
        ImGui::PopStyleColor();
        finPolice();
        police(g_regular, 13);
        ImGui::PushStyleColor(ImGuiCol_Text, rgb(kTexteDoux));
        ImGui::TextUnformatted(tr(T_SousMarque));
        ImGui::PopStyleColor();
        finPolice();
        ImGui::EndGroup();
    }

    ImVec2 tailleFenetre() { return ImGui::GetIO().DisplaySize; }

    // Carte centrale des écrans d'installation ; renvoie la largeur utile
    float carte(const char* id, float haut = 96, float hauteur = 0)
    {
        ImVec2 sz = tailleFenetre();
        float w = S(760), h = hauteur > 0 ? S(hauteur) : sz.y - S(haut) - S(40);
        if (hauteur > 0) haut = (sz.y / g_s - hauteur) * 0.5f + 20;
        debutCarte(id, ImVec2((sz.x - w) * 0.5f, S(haut)), ImVec2(w, h));
        return ImGui::GetContentRegionAvail().x;
    }

    // Boutons en bas de la carte : gauche (secondaire, facultatif) et droite (principal)
    int piedCarte(const char* gauche, const char* droite, bool droiteActif = true, const char* milieu = nullptr, const char* note = nullptr)
    {
        const float y = ImGui::GetWindowHeight() - S(34) - S(44);
        int r = 0;
        if (note)
        {
            police(g_regular, 15);
            ImGui::SetCursorPosY(y + (S(44) - ImGui::GetTextLineHeight()) * 0.5f);
            ImGui::PushStyleColor(ImGuiCol_Text, rgb(kTexteDoux));
            ImGui::TextUnformatted(note);
            ImGui::PopStyleColor();
            finPolice();
        }
        ImGui::SetCursorPosY(y);
        if (gauche && bouton(gauche, false, ImVec2(0, S(44)))) r = 1;
        if (milieu) { ImGui::SameLine(); if (bouton(milieu, false, ImVec2(0, S(44)))) r = 3; }
        if (droite)
        {
            float w = largeurBouton(droite) + S(24);
            if (gauche || milieu) ImGui::SameLine(ImGui::GetWindowWidth() - S(40) - w);
            else ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - S(40) - w, y));
            if (bouton(droite, true, ImVec2(w, S(44)), droiteActif)) r = 2;
        }
        return r;
    }

    std::string decimale(std::string t)   // 41.7 -> 41,7 en français
    {
        if (langueFr()) for (size_t i = 1; i + 1 < t.size(); ++i) if (t[i] == '.' && isdigit((unsigned char)t[i - 1]) && isdigit((unsigned char)t[i + 1])) t[i] = ',';
        return t;
    }

#ifdef _WIN32
    static std::string cheminWin(fs::path p) { std::string d = u8(p); std::replace(d.begin(), d.end(), '/', '\\'); return d; }
#else
    static std::string cheminWin(fs::path p) { return u8(p); }
#endif

    void commencer()
    {
        if (!ouvrirDossier()) return;
        if (ecran == Ecran::Accueil) ecran = Ecran::Choix;
    }

    void ecranAccueil()
    {
        carte("accueil", 0, 470);
        ImGui::Dummy(ImVec2(0, S(8)));
        titre(tr(T_Bienvenue), 34);
        ImGui::Dummy(ImVec2(0, S(10)));
        police(g_regular, 18);
        ImGui::TextWrapped("%s", tr(T_AccueilIntro));
        finPolice();
        ImGui::Dummy(ImVec2(0, S(26)));
        const Texte etapes[] = { T_AccueilEtape1, T_AccueilEtape2, T_AccueilEtape3 };
        for (int i = 0; i < 3; ++i)
        {
            icone(Icone::Numero, i + 1);
            police(g_regular, kTexteTaille);
            ImGui::TextUnformatted(tr(etapes[i]));
            finPolice();
            ImGui::Dummy(ImVec2(0, S(10)));
        }
        if (piedCarte(nullptr, tr(T_Commencer), true, nullptr, tr(T_AccueilNote)) == 2) commencer();
        finCarte();
    }

    void slot(const char* id, const char* nom, const char* aide, Fichier& f, int voulu, float w)
    {
        uint32_t bord = f.p.empty() ? kBord : f.ok ? kVert : kRouge;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, rgb(kChamp, 0.75f));
        ImGui::PushStyleColor(ImGuiCol_Border, rgb(bord, f.p.empty() ? 0.6f : 0.9f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(22), S(20)));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, S(1.5f));
        ImGui::BeginChild(id, ImVec2(w, S(256)), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
        zoneMin[voulu] = ImGui::GetWindowPos();   // zone de dépôt de ce champ
        zoneMax[voulu] = ImVec2(zoneMin[voulu].x + ImGui::GetWindowWidth(), zoneMin[voulu].y + ImGui::GetWindowHeight());
        titre(nom, 21);
        texteDoux(aide, 14);
        ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY() + S(12), S(108)));   // état à la même hauteur dans les deux champs
        if (f.p.empty()) texteCouleur(kTexteDoux, tr(T_AucunFichier));
        else
        {
            // Pastille : ✔ « Jeu de base » / « Mise à jour 2 », ou ✖ et la cause ; nom du fichier en dessous
            icone(f.ok ? Icone::Ok : Icone::Erreur);
            ImGui::BeginGroup();
            ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x);
            texteCouleur(f.ok ? kVert : kRouge, f.ok ? tr(voulu == 0 ? T_JeuDeBase : T_MiseAJour2) : f.erreur.c_str(), g_semibold, 16);
            police(g_regular, 13);
            ImGui::PushStyleColor(ImGuiCol_Text, rgb(kTexteDoux));
            ImGui::TextWrapped("%s", u8(f.p.filename()).c_str());
            ImGui::PopStyleColor();
            finPolice();
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
        }
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - S(20) - S(40));
        if (bouton(fmt("%s##%s", tr(T_Parcourir), id).c_str(), false, ImVec2(0, S(40)))) parcourir(voulu);
        ImGui::EndChild();
    }

    void ecranChoix()
    {
        float w = carte("choix");
        titre(tr(T_ChoixTitre));
        ImGui::Dummy(ImVec2(0, S(4)));
        police(g_regular, kTexteTaille);
        ImGui::PushStyleColor(ImGuiCol_Text, rgb(kTexteDoux));
        ImGui::TextWrapped("%s", tr(T_ChoixIntro));
        ImGui::PopStyleColor();
        finPolice();
        ImGui::Dummy(ImVec2(0, S(18)));
        float sw = (w - S(20)) * 0.5f;
        slot("jeu", tr(T_SlotJeu), tr(T_SlotJeuAide), jeu, 0, sw);
        ImGui::SameLine(0, S(20));
        slot("maj", tr(T_SlotMaj), tr(T_SlotMajAide), maj, 1, sw);
        if (!refus.empty())
        {
            ImGui::Dummy(ImVec2(0, S(10)));
            icone(Icone::Erreur);
            texteCouleur(kRouge, refus.c_str(), nullptr, 15);
        }
        int b = piedCarte(tr(T_Retour), tr(T_Recompiler), jeu.ok && maj.ok);   // grisé tant que les deux champs ne sont pas ✔
        if (b == 1) ecran = Ecran::Accueil;
        if (b == 2) verifier();
        finCarte();
    }

    void ligneEtat(Icone i, const char* t, bool fort = false)
    {
        icone(i);
        police(fort ? g_semibold : g_regular, kTexteTaille);
        ImGui::PushStyleColor(ImGuiCol_Text, rgb(i == Icone::Attente ? kTexteDoux : kTexte));
        ImGui::TextWrapped("%s", t);
        ImGui::PopStyleColor();
        finPolice();
        ImGui::Dummy(ImVec2(0, S(8)));
    }

    // Panneau d'erreur : titre, explication, détail technique et fin du journal
    void panneauErreur(const char* entete, const char* explication, const std::string& erreur, const std::deque<std::string>& fin)
    {
        icone(Icone::Erreur);
        texteCouleur(kRouge, entete, g_semibold, 20);
        ImGui::Dummy(ImVec2(0, S(6)));
        if (explication) { police(g_regular, 16); ImGui::TextWrapped("%s", explication); finPolice(); ImGui::Dummy(ImVec2(0, S(6))); }
        float h = ImGui::GetWindowHeight() - ImGui::GetCursorPosY() - S(34) - S(44) - S(24);
        h = std::min(h, S(26) + (1 + fin.size()) * S(26));   // hauteur du contenu (lignes repliées : barre de défilement)
        ImGui::PushStyleColor(ImGuiCol_ChildBg, rgb(kChamp, 0.85f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(10)));
        ImGui::BeginChild("detail", ImVec2(0, std::max(h, S(60))), ImGuiChildFlags_AlwaysUseWindowPadding);
        police(g_regular, 13);
        ImGui::PushStyleColor(ImGuiCol_Text, rgb(kTexteDoux));
        ImGui::TextWrapped("%s %s", tr(T_Detail), erreur.c_str());
        for (auto& l : fin) ImGui::TextWrapped("%s", l.c_str());
        ImGui::PopStyleColor();
        finPolice();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }

    void ecranVerification()
    {
        carte("verif");
        titre(tr(T_VerifTitre));
        ImGui::Dummy(ImVec2(0, S(18)));
        bool enCours, echec, mauvaise;
        Etape e;
        double frac;
        std::string erreur;
        std::deque<std::string> fin;
        {
            std::lock_guard l(g_t.m);
            enCours = job == Job::Verification, echec = g_t.echec && !dejaPrepare, mauvaise = g_t.mauvaiseCopie;
            e = g_t.etape, frac = g_t.frac, erreur = g_t.erreur, fin = g_t.fin;
        }
        const bool fini = !enCours && (verifOk || dejaPrepare);
        if (!echec) ligneEtat(Icone::Ok, tr(dejaPrepare ? T_VerifDejaPrepare : T_VerifFichiers));
        if (gb >= 0 && !echec)
        {
            std::string t = decimale(fmt(tr(gb < 3 ? T_VerifEspaceBas : T_VerifEspace), gb));
            ligneEtat(gb < 3 ? Icone::Attention : Icone::Ok, t.c_str());
        }
        if (!dejaPrepare && !echec && !fini)   // une fois la copie validée : place pour les cases de l'installation
        {
            auto etat = [&](Etape x) {
                if (fini || (int)e > x) return Icone::Ok;
                if (echec && e == x) return Icone::Erreur;
                return enCours && e == x ? Icone::EnCours : Icone::Attente;
            };
            ligneEtat(etat(EtOutils), tr(T_VerifOutils), enCours && e == EtOutils);
            if (enCours && e == EtOutils) { barre(float(frac), 8); ImGui::Dummy(ImVec2(0, S(8))); }
            ligneEtat(etat(EtPreparation), tr(T_VerifExtraction), enCours && e == EtPreparation);
            if (enCours && e == EtPreparation) { barre(-1, 8); ImGui::Dummy(ImVec2(0, S(8))); }
            ligneEtat(fini ? Icone::Ok : (echec && mauvaise) ? Icone::Erreur : Icone::Attente, tr(T_VerifVersion));
        }
        ImGui::Dummy(ImVec2(0, S(echec ? 0 : 12)));
        if (echec && !manquants.empty())   // Linux : composants du système à installer
        {
            std::string l;
            for (auto& m : manquants) l += (l.empty() ? "" : ", ") + m;
            const bool pacman = pacmanDisponible();
            panneauErreur(tr(T_DepTitre), tr(pacman ? T_DepPacman : T_DepAutre), l, fin);
            int b = piedCarte(tr(T_OuvrirJournal), tr(pacman ? T_Installer : T_Reessayer));
            if (b == 1) ouvrir(journalPath);
            if (b == 2) { manquants.clear(); verifier(pacman); }
        }
        else if (echec)
        {
            panneauErreur(tr(T_VerifEchec), nullptr, erreur, fin);
            int b = piedCarte(tr(T_OuvrirJournal), tr(T_AutresFichiers));
            if (b == 1) ouvrir(journalPath);
            if (b == 2) { jeu = {}, maj = {}; ecran = Ecran::Choix; }
        }
        else if (fini)
        {
            icone(Icone::Ok);
            texteCouleur(kVert, tr(T_VerifOk), g_semibold, 20);
            ImGui::Dummy(ImVec2(0, S(4)));
            police(g_regular, 16);
            ImGui::TextWrapped("%s", tr(T_VerifOkDetail));
            finPolice();
            ImGui::Dummy(ImVec2(0, S(10)));
            casesInstallation();
            if (piedCarte(dejaPrepare ? nullptr : tr(T_Retour), tr(T_LancerRecompilation)) == 2) construire(false);
        }
        finCarte();
    }

    void ecranProgression()
    {
        carte("prog");
        bool enCours, echec;
        Etape e;
        double frac;
        std::string erreur, detail;
        std::deque<std::string> fin;
        double ecoule;
        {
            std::lock_guard l(g_t.m);
            enCours = job == Job::Construction, echec = g_t.echec;
            e = g_t.etape, frac = g_t.frac, erreur = g_t.erreur, fin = g_t.fin, detail = g_t.detail;
            ecoule = std::chrono::duration<double>(Clock::now() - g_t.debutEtape).count();
        }
        titre(tr(echec ? T_ProgEchec : T_ProgTitre));
        ImGui::Dummy(ImVec2(0, S(2)));
        if (!echec) texteDoux(tr(T_ProgIntro), 16);
        ImGui::Dummy(ImVec2(0, S(16)));
        const Texte noms[NbEtapes] = { T_EtOutils, T_EtPreparation, T_EtXenon, T_EtImports, T_EtConfiguration, T_EtCompilation, T_EtInstallation };
        if (echec)
        {
            panneauErreur(tr(noms[e]), nullptr, erreur, fin);
            int b = piedCarte(tr(T_OuvrirJournal), tr(T_Reessayer));
            if (b == 1) ouvrir(journalPath);
            if (b == 2) construire(false);
            finCarte();
            return;
        }
        for (int i = 0; i < NbEtapes; ++i)
        {
            Icone ic = i < e ? Icone::Ok : i == e && enCours ? Icone::EnCours : Icone::Attente;
            icone(ic);
            police(i == e ? g_semibold : g_regular, i == e ? 18 : 16);
            ImGui::PushStyleColor(ImGuiCol_Text, rgb(i < e ? kTexteDoux : i == e ? kTexte : kTexteDoux, i > e ? 0.6f : 1.0f));
            ImGui::TextUnformatted(tr(noms[i]));
            ImGui::PopStyleColor();
            finPolice();
            ImGui::Dummy(ImVec2(0, S(i == e ? 8 : 3)));
            if (i == e)
            {
                ImGui::Indent(S(34));
                barre(float(frac), 12);
                ImGui::Dummy(ImVec2(0, S(4)));
                police(g_regular, 14);
                ImGui::PushStyleColor(ImGuiCol_Text, rgb(kTexteDoux));
                if (frac >= 0) ImGui::Text("%d %%", int(frac * 100));
                if (frac >= 0 && !detail.empty()) { ImGui::SameLine(0, S(14)); ImGui::TextUnformatted(detail.c_str()); }
                ImGui::PopStyleColor();
                finPolice();
                ImGui::Unindent(S(34));
                ImGui::Dummy(ImVec2(0, S(8)));
            }
        }
        // Temps restant : vitesse observée de l'étape en cours (la compilation, dernière étape longue, domine)
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - S(34) - S(30));
        std::string reste = tr(T_Calcul);
        if (frac > 0.02 && ecoule > 5)
        {
            double s = ecoule * (1 - frac) / frac;
            reste = s < 60 ? tr(T_TempsMoinsMin) : fmt(tr(T_TempsMin), int(std::ceil(s / 60)));
        }
        if (e == EtCompilation || frac >= 0) texteCouleur(kOrangeClair, fmt(tr(T_TempsRestant), reste.c_str()).c_str(), g_semibold, 17);
        finCarte();
    }

    // ---- Réglages ----
    bool ligneReglage(const char* nom, const char* aide)
    {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        police(g_regular, 16);
        ImGui::TextUnformatted(nom);
        finPolice();
        if (aide && *aide)
        {
            police(g_regular, 13);
            ImGui::PushStyleColor(ImGuiCol_Text, rgb(kTexteDoux));
            ImGui::TextWrapped("%s", aide);
            ImGui::PopStyleColor();
            finPolice();
        }
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
        return true;
    }

    template <class T> bool liste(const char* id, T* v, const std::vector<std::pair<T, std::string>>& choix)
    {
        std::string cur;
        for (auto& [k, n] : choix) if (k == *v) cur = n;
        bool r = false;
        if (ImGui::BeginCombo(id, cur.c_str()))
        {
            for (auto& [k, n] : choix)
                if (ImGui::Selectable(n.c_str(), k == *v)) *v = k, r = true;
            ImGui::EndCombo();
        }
        return r;
    }

    bool debutTable()
    {
        return ImGui::BeginTable("r", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX, ImVec2(0, 0));
    }
    void colonnes()
    {
        ImGui::TableSetupColumn("n", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthFixed, S(300));
    }

    void ongletAffichage()
    {
        if (!debutTable()) return;
        colonnes();
        ligneReglage(tr(T_Mode), tr(T_ModeAide));
        liste<std::string>("##mode", &c.mode, { { "fenetre", tr(T_ModeFenetre) }, { "plein_ecran", tr(T_ModePleinEcran) }, { "sans_bordure", tr(T_ModeSansBordure) } });
        ligneReglage(tr(T_Ecran), tr(T_EcranAide));
        {
            std::vector<std::pair<int, std::string>> t;
            const int n = std::max(SDL_GetNumVideoDisplays(), c.screen + 1);
            for (int i = 0; i < n; ++i)
            {
                const char* nom = SDL_GetDisplayName(i);
                t.push_back({ i, fmt("%d : %s", i + 1, nom ? nom : "?") });
            }
            if (liste<int>("##ecran", &c.screen, t)) c.posX.reset(), c.posY.reset();   // autre écran : fenêtre centrée
        }
        ligneReglage(tr(T_Taille), tr(T_TailleAide));
        {
            std::vector<std::pair<int, std::string>> t;
            const int tailles[][2] = { { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 } };
            bool connue = false;
            for (auto& x : tailles) connue |= x[0] == c.width && x[1] == c.height;
            if (!connue) t.push_back({ c.width * 100000 + c.height, fmt("%d x %d", c.width, c.height) });
            for (auto& x : tailles) t.push_back({ x[0] * 100000 + x[1], fmt("%d x %d", x[0], x[1]) });
            int v = c.width * 100000 + c.height;
            if (liste<int>("##taille", &v, t)) c.width = v / 100000, c.height = v % 100000;
        }
        ligneReglage(tr(T_Vsync), tr(T_VsyncAide));
        ImGui::Checkbox("##vsync", &c.vsync);
        ligneReglage(tr(T_Limite), tr(T_LimiteAide));
        {
            std::vector<std::pair<int, std::string>> t = { { 0, tr(T_Aucune) } };
            const int l[] = { 30, 60, 120, 144, 165, 240 };
            bool connue = c.fpsLimit == 0;
            for (int x : l) connue |= x == c.fpsLimit;
            if (!connue) t.push_back({ c.fpsLimit, fmt("%d", c.fpsLimit) });
            for (int x : l) t.push_back({ x, fmt("%d", x) });
            liste<int>("##limite", &c.fpsLimit, t);
        }
        ligneReglage(tr(T_AfficherIps), tr(T_AfficherIpsAide));
        ImGui::Checkbox("##ips", &c.showFps);
        ligneReglage(tr(T_Gpu), tr(T_GpuAide));
        {
            char b[128];
            std::snprintf(b, sizeof b, "%s", c.gpu.c_str());
            if (ImGui::InputTextWithHint("##gpu", tr(T_GpuAuto), b, sizeof b)) c.gpu = b;
        }
        ligneReglage(tr(T_Echelle), tr(T_EchelleAide));
        liste<int>("##echelle", &c.renderScale, { { 1, "1280x720" }, { 2, "2560x1440" }, { 3, "3840x2160" }, { 4, "5120x2880" } });
        ImGui::EndTable();
    }

    void ongletImage()
    {
        if (!debutTable()) return;
        colonnes();
        ligneReglage(tr(T_Prereglage), tr(T_PrereglageAide));
        {
            std::string p = cfg::Prereglage(c);   // déduit des réglages : tout changement à la main donne « personnalise »
            std::vector<std::pair<std::string, std::string>> t = { { "auto", tr(T_MsaaAuto) }, { "economie", tr(T_Economie) },
                { "equilibre", tr(T_Equilibre) }, { "qualite", tr(T_Qualite) } };
            if (p == "personnalise") t.push_back({ p, tr(T_Personnalise) });
            if (liste<std::string>("##prereglage", &p, t)) cfg::AppliquerPrereglage(p, &c);
        }
        ligneReglage(tr(T_Anticrenelage), tr(T_AnticrenelageAide));
        liste<std::string>("##aa", &c.antiAliasing, { { "aucun", tr(T_AaAucun) }, { "fxaa", "FXAA" } });
        ligneReglage(tr(T_Msaa), tr(T_MsaaAide));
        liste<std::string>("##msaa", &c.msaa, { { "auto", tr(T_MsaaAuto) }, { "desactive", tr(T_Desactive) }, { "2x", "x2" }, { "4x", "x4" }, { "8x", "x8" },
            { "active", tr(T_Active) } });
        ligneReglage(tr(T_Anisotrope), tr(T_AnisotropeAide));
        liste<int>("##aniso", &c.anisotropy, { { 1, "x1" }, { 2, "x2" }, { 4, "x4" }, { 8, "x8" }, { 16, "x16" } });
        ligneReglage(tr(T_Ombres), tr(T_OmbresAide));
        liste<std::string>("##ombres", &c.ombres, { { "origine", tr(T_Origine) }, { "x2", "x2" }, { "x4", "x4" } });
        ligneReglage(tr(T_Agrandissement), tr(T_AgrandissementAide));
        liste<std::string>("##agr", &c.upscale, { { "lineaire", tr(T_AgrLineaire) }, { "fsr1", "AMD FSR 1" } });
        ligneReglage(tr(T_Nettete), tr(T_NetteteAide));
        ImGui::SliderFloat("##nettete", &c.sharpness, 0.0f, 1.0f, "%.2f");
        ligneReglage(tr(T_Luminosite), tr(T_LuminositeAide));
        ImGui::SliderFloat("##lum", &c.brightness, 0.5f, 2.0f, "%.2f");
        ligneReglage(tr(T_Gamma), tr(T_GammaAide));
        ImGui::SliderFloat("##gamma", &c.gamma, 0.5f, 3.0f, "%.2f");
        ligneReglage(tr(T_CouleurXbox), tr(T_CouleurXboxAide));
        ImGui::Checkbox("##xbox", &c.xboxColor);
        ImGui::EndTable();
    }

    void ongletSon()
    {
        if (!debutTable()) return;
        colonnes();
        ligneReglage(tr(T_Volume), tr(T_VolumeAide));
        int v = int(std::lround(c.volume * 100));
        if (ImGui::SliderInt("##vol", &v, 0, 100, "%d %%")) c.volume = v / 100.0f;
        ligneReglage(tr(T_CouperFocus), tr(T_CouperFocusAide));
        ImGui::Checkbox("##focus", &c.muteUnfocused);
        ligneReglage(tr(T_Vibrations), tr(T_VibrationsAide));
        {
            int vib = int(std::lround(c.vibration * 100));
            if (ImGui::SliderInt("##vib", &vib, 0, 100, "%d %%")) c.vibration = vib / 100.0f;
        }
        ligneReglage(tr(T_ZoneMorte), tr(T_ZoneMorteAide));
        {
            int z = int(std::lround(c.deadZone * 100));
            if (ImGui::SliderInt("##zone", &z, 0, 50, "%d %%")) c.deadZone = z / 100.0f;
        }
        ligneReglage(tr(T_ArrierePlan), tr(T_ArrierePlanAide));
        ImGui::Checkbox("##fond", &c.backgroundInput);
        for (int j = 0; j < 2; ++j) {
            ligneReglage(tr(j ? T_Joueur2 : T_Joueur1), tr(T_JoueursAide));
            std::vector<std::pair<std::string, std::string>> t;
            for (int n = 1; n <= 4; ++n) t.push_back({ std::to_string(n), fmt(tr(T_ManetteN), n) });
            t.push_back({ "clavier", tr(T_OngletClavier) });
            const std::string avant = c.joueurs[j];
            if (liste<std::string>(j ? "##joueur2" : "##joueur1", &c.joueurs[j], t) && c.joueurs[j] == c.joueurs[1 - j])
                c.joueurs[1 - j] = avant;   // même commande que l'autre joueur : échange
        }
        ligneReglage(tr(T_PasserLogos), tr(T_PasserLogosAide));
        ImGui::Checkbox("##logos", &c.skipLogos);
        ligneReglage(tr(T_ToucheCapture), tr(T_ToucheCaptureAide));
        {
            std::vector<std::pair<std::string, std::string>> t;
            for (int i = 1; i <= 12; ++i) t.push_back({ fmt("F%d", i), fmt("F%d", i) });
            t.push_back({ "PrintScreen", "PrintScreen" });
            bool connue = false;
            for (auto& x : t) connue |= x.first == c.captureKey;
            if (!connue) t.insert(t.begin(), { c.captureKey, c.captureKey });   // touche écrite à la main dans config.toml
            liste<std::string>("##capture", &c.captureKey, t);
        }
        ligneReglage(tr(T_Langue), tr(T_LangueAide));
        liste<std::string>("##langue", &c.langue, { { "auto", tr(T_LangueAuto) }, { "fr", "Français" }, { "en", "English" },
            { "de", "Deutsch" }, { "es", "Español" }, { "it", "Italiano" }, { "ja", "Japanese" } });
        ligneReglage(tr(T_Console), tr(T_ConsoleAide));
        ImGui::Checkbox("##console", &c.console);
        ImGui::EndTable();
    }

    void ongletClavier()
    {
        texteDoux(tr(T_ClavierAide), 14);
        ImGui::Dummy(ImVec2(0, S(4)));
        if (!ImGui::BeginTable("touches", 4, ImGuiTableFlags_SizingStretchProp)) return;
        ImGui::TableSetupColumn("c1", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("t1", ImGuiTableColumnFlags_WidthFixed, S(170));
        ImGui::TableSetupColumn("c2", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("t2", ImGuiTableColumnFlags_WidthFixed, S(170));
        const int n = int(std::size(kCommandes)), moitie = (n + 1) / 2;
        for (int r = 0; r < moitie; ++r)
        {
            ImGui::TableNextRow();
            for (int k : { r, r + moitie })
            {
                ImGui::TableNextColumn();
                if (k >= n) { ImGui::TableNextColumn(); continue; }
                const Commande& cm = kCommandes[k];
                police(g_regular, 15);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(tr(cm.texte));
                finPolice();
                ImGui::TableNextColumn();
                auto it = c.keys.find(cm.nom);
                std::string lib = attenteTouche == k ? tr(T_AppuyerTouche)
                                : it != c.keys.end() ? it->second : fmt(tr(T_ParDefaut), cm.defaut);
                bool perso = it != c.keys.end();
                float wb = perso ? ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - S(4) : -FLT_MIN;
                ImGui::PushStyleColor(ImGuiCol_Button, rgb(attenteTouche == k ? kOrangeFonce : perso ? kSecondaireClair : kChamp));
                police(perso ? g_semibold : g_regular, 14);
                if (ImGui::Button(fmt("%s##k%d", lib.c_str(), k).c_str(), ImVec2(wb, 0))) attenteTouche = attenteTouche == k ? -1 : k;
                finPolice();
                ImGui::PopStyleColor();
                if (perso)
                {
                    ImGui::SameLine(0, S(4));
                    if (ImGui::Button(fmt("\xC3\x97##r%d", k).c_str(), ImVec2(ImGui::GetFrameHeight(), 0))) c.keys.erase(cm.nom);   // ×
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr(T_Reinitialiser));
                }
            }
        }
        ImGui::EndTable();
    }

    void ecranReglages()
    {
        carte("reglages", 40);
        titre(tr(T_Reglages));
        if (finRecompilation) { ImGui::Dummy(ImVec2(0, S(2))); icone(Icone::Ok); texteCouleur(kVert, tr(T_ProgFini), nullptr, 16); }
        if (!erreurConfig.empty()) { icone(Icone::Erreur); texteCouleur(kRouge, erreurConfig.c_str(), nullptr, 15); }
        {   // dossier des données
            const char* ouvrirTxt = tr(T_OuvrirDossier);
            float wb = largeurBouton(ouvrirTxt);
            police(g_regular, 14);
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, rgb(kTexteDoux));
            ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x - wb - S(16));
            ImGui::TextWrapped("%s", fmt(tr(T_Donnees), cheminWin(data).c_str()).c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            finPolice();
            ImGui::SameLine(ImGui::GetContentRegionMax().x - wb);
            if (bouton(ouvrirTxt, false)) ouvrir(data);
        }
        ImGui::Dummy(ImVec2(0, S(4)));
        if (ImGui::BeginTabBar("onglets"))
        {
            const Texte noms[] = { T_OngletAffichage, T_OngletImage, T_OngletSonManette, T_OngletClavier };
            for (int i = 0; i < 4; ++i)
            {
                police(g_semibold, 16);
                bool ouvert = ImGui::BeginTabItem(tr(noms[i]), nullptr, ongletForce == i ? ImGuiTabItemFlags_SetSelected : 0);
                finPolice();
                if (!ouvert) continue;
                if (i != 3) attenteTouche = -1;
                float h = ImGui::GetWindowHeight() - ImGui::GetCursorPosY() - S(34) - S(44) - S(16);
                ImGui::BeginChild("defil", ImVec2(0, h));
                ImGui::Dummy(ImVec2(0, S(6)));
                if (i == 0) ongletAffichage();
                if (i == 1) ongletImage();
                if (i == 2) ongletSon();
                if (i == 3) ongletClavier();
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        int b = piedCarte(tr(T_Annuler), tr(finRecompilation ? T_Continuer : T_Enregistrer), true, tr(T_Defauts));
        if (b == 3) c = cfg::Config();
        if (b == 1) { attenteTouche = -1; chargerReglages(); erreurConfig.clear(); ecran = retourReglages; finRecompilation = false; }
        if (b == 2 && enregistrerReglages()) { attenteTouche = -1; ecran = retourReglages; finRecompilation = false; }
        finCarte();
    }

    void ecranJouer()
    {
        ImVec2 sz = tailleFenetre();
        const float m = S(40);
        // Illustration du jeu (copie du joueur)
        float hA = sz.y - 2 * m, wA = keyart.tex ? hA * keyart.w / keyart.h : S(420);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 a(m, m), b(m + wA, m + hA);
        dl->AddRectFilled({ a.x + S(6), a.y + S(10) }, { b.x + S(6), b.y + S(10) }, u32(0x000000, 0.35f), S(18));   // ombre
        if (keyart.tex) dl->AddImageRounded(ImTextureRef(ImTextureID(intptr_t(keyart.tex))), a, b, { 0, 0 }, { 1, 1 }, IM_COL32_WHITE, S(18));
        else dl->AddRectFilledMultiColor(a, b, u32(kOrangeFonce), u32(kOrange), u32(kFondBas), u32(kFondHaut));
        dl->AddRect(a, b, u32(kOrangeClair, 0.35f), S(18), 0, S(1.5f));

        float x = b.x + S(56), w = sz.x - x - m;
        ImGui::SetCursorPos(ImVec2(x, S(110)));
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(x + w);
        police(g_semibold, 15);
        ImGui::PushStyleColor(ImGuiCol_Text, rgb(kOrangeClair));
        ImGui::TextUnformatted("WAKFURECOMP");
        ImGui::PopStyleColor();
        finPolice();
        police(g_semibold, 52);
        ImGui::TextUnformatted("Islands of Wakfu");
        finPolice();
        police(g_regular, 18);
        ImGui::PushStyleColor(ImGuiCol_Text, rgb(kTexteDoux));
        ImGui::TextWrapped("%s", tr(T_SousTitreJeu));
        ImGui::PopStyleColor();
        finPolice();
        ImGui::Dummy(ImVec2(0, S(40)));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(16));
        ImGui::PushStyleColor(ImGuiCol_Button, rgb(kOrange));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rgb(kOrangeClair));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, rgb(kOrangeFonce));
        ImGui::PushStyleColor(ImGuiCol_Text, rgb(0x2A1406));
        police(g_semibold, 28);
        if (ImGui::Button(tr(T_Jouer), ImVec2(S(320), S(76)))) jouer();
        finPolice();
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();
        ImGui::Dummy(ImVec2(0, S(14)));
        float bw = (S(320) - S(12)) * 0.5f;
        if (bouton(tr(T_Reglages), false, ImVec2(bw, S(46)))) { chargerReglages(); retourReglages = Ecran::Jouer; ecran = Ecran::Reglages; }
        ImGui::SameLine(0, S(12));
        if (bouton(tr(T_Recompiler), false, ImVec2(bw, S(46)))) confirmerRecompilation = true;
        ImGui::Dummy(ImVec2(0, S(2)));
        if (bouton(tr(T_Desinstaller), false, ImVec2(S(320), S(40)))) preparerDesinstallation();
        ImGui::Dummy(ImVec2(0, S(2)));
        if (bouton(tr(T_Restaurer), false, ImVec2(S(320), S(40))))
        {
            copies = copiesSecours(data);
            copieChoisie = copies.empty() ? "" : copies.front();
            if (copies.empty()) messageSecours = tr(T_AucuneCopie);
            else confirmerRestauration = true;
        }
        if (!messageSecours.empty()) texteDoux(messageSecours.c_str(), 14);
        ImGui::Dummy(ImVec2(0, S(26)));
        if (!aJour && messageJeu.empty()) { icone(Icone::Attention); texteCouleur(kJaune, tr(T_NouvelleVersion), nullptr, 15); }
        if (!messageJeu.empty())
        {
            icone(Icone::Erreur);
            ImGui::BeginGroup();
            texteCouleur(kRouge, messageJeu.c_str(), g_semibold, 16);
            fs::path j = data / "journal.log";
            std::error_code ec;
            if (fs::exists(j, ec) && bouton(tr(T_VoirJournalJeu), false)) ouvrir(j);
            ImGui::EndGroup();
        }
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        ImGui::SetCursorPos(ImVec2(x, sz.y - m - S(20)));
        ImGui::PushTextWrapPos(x + w);
        texteDoux(fmt(tr(T_Donnees), cheminWin(data).c_str()).c_str(), 13);
        ImGui::PopTextWrapPos();
    }

    // Étapes de la fenêtre (en haut à droite) : Jeu -> Installation -> Jouer
    void etapes()
    {
        const int cur = ecran == Ecran::Verification || ecran == Ecran::Progression ? 1 : 0;
        const Texte noms[3] = { T_EtapeJeu, T_EtapeInstallation, T_EtapeJouer };
        police(g_semibold, 14);
        float w = 0;
        for (int i = 0; i < 3; ++i) w += ImGui::CalcTextSize(fmt("%d  %s", i + 1, tr(noms[i])).c_str()).x + (i ? S(40) : 0);
        ImVec2 p(tailleFenetre().x - S(40) - w, S(38));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 o = ImGui::GetWindowPos();
        for (int i = 0; i < 3; ++i)
        {
            std::string t = fmt("%d  %s", i + 1, tr(noms[i]));
            uint32_t c = i < cur ? kVert : i == cur ? kOrangeClair : kTexteDoux;
            dl->AddText(ImVec2(o.x + p.x, o.y + p.y), u32(c, i > cur ? 0.6f : 1.0f), t.c_str());
            p.x += ImGui::CalcTextSize(t.c_str()).x;
            if (i < 2) dl->AddLine(ImVec2(o.x + p.x + S(10), o.y + p.y + S(9)), ImVec2(o.x + p.x + S(30), o.y + p.y + S(9)), u32(kBord), S(1.5f));
            p.x += S(40);
        }
        finPolice();
    }

    void caseGarder()
    {
        police(g_regular, 15);
        ImGui::Checkbox(tr(T_GarderCompilation), &garderCompilation);
        finPolice();
        ImGui::Indent(S(36));
        texteDoux(tr(T_GarderCompilationAide), 13);
        ImGui::Unindent(S(36));
    }

    void casesInstallation()   // avant la recompilation : ménage, icône du bureau
    {
        caseGarder();
        police(g_regular, 15);
        ImGui::Checkbox(tr(T_IconeBureau), &iconeBureau);
        finPolice();
    }

    std::string taille(uint64_t o) { return decimale(fmt(tr(T_TailleGo), o / 1073741824.0)); }

    // ---- Anciennes installations (proposées une fois) ----
    void finAnciennes()
    {
        anciennesRepondu = true;
        anciennes.clear();
        demarrerSuite();
    }

    void ecranAnciennes()
    {
        carte("anciennes");
        titre(tr(T_AnciennesTitre));
        ImGui::Dummy(ImVec2(0, S(6)));
        texteDoux(tr(T_AnciennesIntro), 16);
        ImGui::Dummy(ImVec2(0, S(14)));
        bool enCours, echec;
        std::string erreur;
        std::deque<std::string> fin;
        { std::lock_guard l(g_t.m); enCours = job == Job::Suppression; echec = g_t.echec; erreur = g_t.erreur; fin = g_t.fin; }
        for (auto& a : anciennes)
            ligneEtat(enCours ? Icone::EnCours : Icone::Attention, (cheminWin(a.dossier) + "   " + taille(a.taille)).c_str());
        if (echec && !enCours) panneauErreur(fmt(tr(T_ErrSuppression), "").c_str(), nullptr, erreur, fin);
        if (enCours) { piedCarte(nullptr, nullptr, false, nullptr, tr(T_DesinstEnCours)); finCarte(); return; }
        int b = piedCarte(tr(T_PasMaintenant), tr(T_Supprimer));
        if (b == 1) finAnciennes();
        if (b == 2) supprimerAnciennes();
        finCarte();
    }

    void supprimerAnciennes() { lancer(Job::Suppression, [liste = anciennes] { for (auto& a : liste) supprimerAncienne(a); }); }

    // ---- Désinstallation ----
    void listerDesinstallation()
    {
        listeDesinst.clear();
        for (auto& p : aDesinstaller(in, !effacerSauvegardes)) listeDesinst.push_back({ p, tailleSansLiens(p) });
    }

    void preparerDesinstallation()
    {
        effacerSauvegardes = false;
        { std::lock_guard l(g_t.m); g_t.echec = false; g_t.erreur.clear(); g_t.fin.clear(); }
        listerDesinstallation();
        ecran = Ecran::Desinstaller;
    }

    void ecranDesinstaller()
    {
        carte("desinstaller");
        titre(tr(T_DesinstTitre));
        ImGui::Dummy(ImVec2(0, S(10)));
        bool enCours, echec;
        std::string erreur;
        std::deque<std::string> fin;
        { std::lock_guard l(g_t.m); enCours = job == Job::Suppression; echec = g_t.echec; erreur = g_t.erreur; fin = g_t.fin; }
        if (echec && !enCours)
        {
            panneauErreur(fmt(tr(T_ErrSuppression), "").c_str(), nullptr, erreur, fin);
            if (piedCarte(nullptr, tr(T_Fermer)) == 2) quitter = true;
            finCarte();
            return;
        }
        police(g_semibold, 16);
        ImGui::TextUnformatted(tr(T_DesinstIntro));
        finPolice();
        float h = ImGui::GetWindowHeight() - ImGui::GetCursorPosY() - S(34) - S(44) - S(150);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, rgb(kChamp, 0.85f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(10)));
        ImGui::BeginChild("liste", ImVec2(0, std::max(h, S(80))), ImGuiChildFlags_AlwaysUseWindowPadding);
        police(g_regular, 14);
        for (auto& [p, t] : listeDesinst)
        {
            ImGui::TextUnformatted(cheminWin(p).c_str());
            if (t >= (1u << 20)) { ImGui::SameLine(); ImGui::TextColored(rgb(kTexteDoux), "%s", taille(t).c_str()); }
        }
        finPolice();
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, S(6)));
        ImGui::BeginDisabled(enCours);
        police(g_regular, 15);
        bool v = effacerSauvegardes;
        if (ImGui::Checkbox(tr(T_DesinstSauvegardes), &v))
        {
            if (v) confirmerSauvegardes = true;   // confirmation (popup « sauvegardes ») avant de cocher
            else { effacerSauvegardes = false; listerDesinstallation(); }
        }
        finPolice();
        ImGui::EndDisabled();
        if (!effacerSauvegardes) texteDoux(fmt(tr(T_DesinstGardeSauv), cheminWin(data).c_str()).c_str(), 14);
        texteDoux(tr(T_DesinstGarde), 14);
        if (enCours) { piedCarte(nullptr, nullptr, false, nullptr, tr(T_DesinstEnCours)); finCarte(); return; }
        int b = piedCarte(tr(T_Annuler), tr(T_Desinstaller));
        if (b == 1) ecran = Ecran::Jouer;
        if (b == 2) lancerDesinstallation();
        finCarte();
    }

    void lancerDesinstallation()
    {
        {
            journal("désinstallation" + std::string(effacerSauvegardes ? " (sauvegardes comprises)" : "") + " :");
            for (auto& [p, t] : listeDesinst) journal("  " + u8(p));
            {   // le journal du lanceur est dans le dossier supprimé : la suite reste en mémoire
                std::lock_guard l(g_t.m);
                if (g_t.journal) std::fclose(g_t.journal);
                g_t.journal = nullptr;
            }
            lancer(Job::Suppression, [this, garder = !effacerSauvegardes] { desinstaller(in, garder); });
        }
    }

    void ecranDesinstalle()
    {
        size_t gardes;   // fichiers d'entrée gardés, affichés plus bas : carte agrandie d'autant
        { std::lock_guard l(g_t.m); gardes = g_t.fin.size(); }
        carte("desinstalle", 0, std::min(360.f + 56.f * gardes, tailleFenetre().y / g_s - 40));
        icone(Icone::Ok);
        texteCouleur(kVert, tr(T_DesinstFini), g_semibold, 24);
        ImGui::Dummy(ImVec2(0, S(14)));
        police(g_regular, 16);
        ImGui::TextWrapped("%s", fmt(tr(T_DesinstFiniDetail), cheminWin(selfPath()).c_str()).c_str());
        if (!effacerSauvegardes) { ImGui::Dummy(ImVec2(0, S(8))); ImGui::TextWrapped("%s", fmt(tr(T_DesinstGardeSauv), cheminWin(data).c_str()).c_str()); }
        {   // seules lignes de desinstaller() : fichiers d'entrée gardés
            std::lock_guard l(g_t.m);
            for (auto& f : g_t.fin) { ImGui::Dummy(ImVec2(0, S(4))); texteCouleur(kJaune, f.c_str()); }
        }
        finPolice();
        if (piedCarte(nullptr, tr(T_Fermer)) == 2) quitter = true;
        finCarte();
    }

    void ecranFatale()
    {
        carte("fatale");
        titre(tr(T_ErreurTitre));
        ImGui::Dummy(ImVec2(0, S(12)));
        police(g_regular, 16);
        ImGui::TextWrapped("%s", tr(T_ErreurDemarrage));
        ImGui::Dummy(ImVec2(0, S(6)));
        ImGui::PushStyleColor(ImGuiCol_Text, rgb(kRouge));
        ImGui::TextWrapped("%s", fatale.c_str());
        ImGui::PopStyleColor();
        finPolice();
        int b = piedCarte(tr(T_OuvrirJournal), tr(T_Fermer));
        if (b == 1) ouvrir(journalPath);
        if (b == 2) quitter = true;
        finCarte();
    }

    bool popup(const char* id, const char* titreP, const char* texte, const char* oui, bool* ouvert, bool garder = false,
               const std::function<void()>& contenu = nullptr)
    {
        if (*ouvert) ImGui::OpenPopup(id);
        bool r = false;
        ImVec2 sz = tailleFenetre();
        ImGui::SetNextWindowPos(ImVec2(sz.x * 0.5f, sz.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(S(520), 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(32), S(28)));
        if (ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize))
        {
            titre(titreP, 22);
            ImGui::Dummy(ImVec2(0, S(8)));
            police(g_regular, 16);
            ImGui::TextWrapped("%s", texte);
            finPolice();
            if (garder) { ImGui::Dummy(ImVec2(0, S(8))); caseGarder(); }
            if (contenu) { ImGui::Dummy(ImVec2(0, S(8))); contenu(); }
            ImGui::Dummy(ImVec2(0, S(18)));
            if (bouton(tr(T_Annuler), false, ImVec2(0, S(42)))) { *ouvert = false; ImGui::CloseCurrentPopup(); }
            float w = largeurBouton(oui) + S(24);
            ImGui::SameLine(ImGui::GetWindowWidth() - S(32) - w);
            if (bouton(oui, true, ImVec2(w, S(42)))) { *ouvert = false; r = true; ImGui::CloseCurrentPopup(); }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
        return r;
    }

    void image()
    {
        dessinerFond();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(tailleFenetre());
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("racine", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::PopStyleVar();
        if (ecran != Ecran::Jouer && ecran != Ecran::Reglages) entete();
        if (ecran == Ecran::Accueil || ecran == Ecran::Choix || ecran == Ecran::Verification || ecran == Ecran::Progression) etapes();
        switch (ecran)
        {
        case Ecran::Accueil: ecranAccueil(); break;
        case Ecran::Choix: ecranChoix(); break;
        case Ecran::Verification: ecranVerification(); break;
        case Ecran::Progression: ecranProgression(); break;
        case Ecran::Reglages: ecranReglages(); break;
        case Ecran::Jouer: ecranJouer(); break;
        case Ecran::Fatale: ecranFatale(); break;
        case Ecran::Anciennes: ecranAnciennes(); break;
        case Ecran::Desinstaller: ecranDesinstaller(); break;
        case Ecran::Desinstalle: ecranDesinstalle(); break;
        }
        if (popup("recomp", tr(T_ConfirmRecompTitre), tr(T_ConfirmRecomp), tr(T_Recompiler), &confirmerRecompilation, true)) construire(true);
        if (popup("sauvegardes", tr(T_ConfirmSauvTitre), tr(T_ConfirmSauv), tr(T_Supprimer), &confirmerSauvegardes)) { effacerSauvegardes = true; listerDesinstallation(); }
        if (popup("restaurer", tr(T_RestaurerTitre), tr(T_RestaurerConfirm), tr(T_RestaurerOui), &confirmerRestauration, false, [this] {
                ImGui::SetNextItemWidth(-FLT_MIN);
                std::vector<std::pair<std::string, std::string>> t;
                for (auto& n : copies) t.push_back({ n, n.substr(0, 10) + "  " + n.substr(11, 2) + ":" + n.substr(14, 2) + ":" + n.substr(17, 2) });
                liste<std::string>("##copie", &copieChoisie, t);
            }))
        {
            try
            {
                restaurerSecours(data, copieChoisie);
                messageSecours = fmt(tr(T_Restauree), copieChoisie.c_str());
                journal("sauvegardes restaurées depuis " + u8(data / "sauvegardes-secours" / copieChoisie));
            }
            catch (const std::exception& e)
            {
                messageSecours = fmt(tr(T_ErrRestauration), e.what());
                journal(messageSecours);
            }
        }
        if (popup("quitter", tr(T_QuitterTitre), tr(T_Quitter), tr(T_QuitterOui), &confirmerQuitter)) quitter = true;
        ImGui::End();
    }

    // Dossier de travail, journal, données
    bool ouvrirDossier()
    {
        try
        {
            openWorkDir(in, defaultWorkDir());
            journalPath = in.D / "journal-lanceur.txt";
            {
                std::lock_guard l(g_t.m);
                if (g_t.journal) std::fclose(g_t.journal);
                g_t.journal = ouvrirJournal(journalPath);
            }
            journal("==== WakfuRecomp (fenêtre), outils " + in.id);
            {
                std::lock_guard l(g_t.m);
                if (g_t.journal && !g_t.avantJournal.empty()) { std::fputs(g_t.avantJournal.c_str(), g_t.journal); std::fflush(g_t.journal); }
                g_t.avantJournal.clear();
            }
            data = dataDir(in.D);
            if (!fs::exists(raccourciMenu()))   // entrée de menu (Démarrer / applications)
                try { creerRaccourci(raccourciMenu(), in); journal("entrée de menu : " + u8(raccourciMenu())); }
                catch (const std::exception& e) { journal(std::string("ERREUR : ") + e.what()); }
            chargerReglages();
            aJour = isUpToDate(in);
            if (anciennesRepondu) std::ofstream(in.D / ".anciennes-vues").close();
            if (isInstalled(in)) { ecran = Ecran::Jouer; chargerImages(); }
            else if (isPrepared(in)) { ecran = Ecran::Verification; dejaPrepare = true; gb = freeGb(in.D); chargerImages(); }
            return true;
        }
        catch (const std::exception& e)
        {
            fatale = e.what();
            ecran = Ecran::Fatale;
            if (journalPath.empty())
            {
                std::error_code ec;
                journalPath = fs::temp_directory_path(ec) / "WakfuRecomp-lanceur.txt";
                std::lock_guard l(g_t.m);
                if (!ec) g_t.journal = ouvrirJournal(journalPath);
            }
            journal(std::string("ERREUR au démarrage : ") + e.what());
            return false;
        }
    }

    void demarrer()
    {
        try { anciennes = anciennesInstallations(); }
        catch (const std::exception& e) { journal(std::string("anciennes installations : ") + e.what()); }
        std::error_code ec;
        if (!anciennes.empty() && !fs::exists(defaultWorkDir() / ".anciennes-vues", ec))   // proposées une fois
        {
            for (auto& a : anciennes) journal(fmt("ancienne installation : %s (%.1f Go)", u8(a.dossier).c_str(), a.taille / 1073741824.0));
            ecran = Ecran::Anciennes;
            return;
        }
        demarrerSuite();
    }

    void demarrerSuite()
    {
        // Premier lancement : accueil, avant de créer quoi que ce soit
        bool premier = false;
        try { premier = !fs::exists(defaultWorkDir() / ".wakfurecomp"); }
        catch (const std::exception&) {}   // LOCALAPPDATA absent : erreur affichée par ouvrirDossier
        if (!premier) { ouvrirDossier(); return; }
        journal("premier lancement : accueil");
    }

    // ---- Capture d'écrans (tests) : WAKFURECOMP_CAPTURE=<dossier> ----
    struct Pas { std::string nom; std::function<void()> prep; std::function<bool()> attendre; };
    std::vector<Pas> pas;
    size_t pasCourant = 0;
    int images = 0;
    fs::path dossierCapture;
    Clock::time_point dernierePeriodique;
    int periodique = 0;

    void enregistrerCapture(const std::string& nom)
    {
        int w, h;
        SDL_GetRendererOutputSize(ren, &w, &h);
        std::vector<uint8_t> px(size_t(w) * h * 4);
        if (SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_RGBA32, px.data(), w * 4) != 0) { journal(std::string("capture : ") + SDL_GetError()); return; }
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
        fs::path p = dossierCapture / ((langueFr() ? "fr-" : "en-") + nom + ".png");
        if (!WritePng(u8(p), uint32_t(w), uint32_t(h), px)) journal("capture : écriture impossible " + u8(p));
    }

    void faux(Etape e, double f, double ecoule, const char* detail = "")   // état simulé (capture sans recompilation réelle)
    {
        std::lock_guard l(g_t.m);
        g_t.etape = e, g_t.frac = f, g_t.detail = detail, g_t.debutEtape = Clock::now() - std::chrono::seconds(int(ecoule));
        g_t.echec = false, g_t.mauvaiseCopie = false;
    }

    void preparerCapture(const fs::path& d)
    {
        dossierCapture = d;
        fs::create_directories(d);
        auto env = [](const char* n) { const char* v = std::getenv(n); return v ? fromU8(v) : fs::path(); };
        fs::path fJeu = env("WAKFURECOMP_CAPTURE_JEU"), fMaj = env("WAKFURECOMP_CAPTURE_TU"), fMauvais = env("WAKFURECOMP_CAPTURE_MAUVAIS");
        const bool reel = std::getenv("WAKFURECOMP_CAPTURE_REEL") != nullptr;
        auto aucun = [] { return true; };
        pas.push_back({ "00-demarrage", [] {}, aucun });   // écran choisi au démarrage (état réel du dossier de travail)
        if (std::getenv("WAKFURECOMP_CAPTURE_ICONE")) iconeBureau = true;
        if (reel && ecran == Ecran::Anciennes)   // anciennes installations : « Supprimer » pour de vrai
        {
            pas.push_back({ "00b-anciennes-suppression", [this] { supprimerAnciennes(); }, [this] { return job == Job::Aucun; } });
            pas.push_back({ "00c-apres-anciennes", [] {}, aucun });
        }
        if (reel && std::getenv("WAKFURECOMP_CAPTURE_INSTALLE"))   // jeu déjà installé : jeu et désinstallation seulement
        {
            capturesFinales(true);
            return;
        }
        pas.push_back({ "01-accueil", [this] { ecran = Ecran::Accueil; }, aucun });
        pas.push_back({ "02-choix-vide", [this] {
            if (in.D.empty()) ouvrirDossier();   // premier lancement : installation normale (capture seulement)
            ecran = Ecran::Choix; jeu = maj = {};
        }, aucun });
        pas.push_back({ "03-choix-mauvais-fichier", [this, fMauvais, fMaj] { choisirFichier(fMauvais, 0); choisirFichier(fMaj, -1, true); choisirFichier(fMauvais, -1, true); }, aucun });
        // WAKFURECOMP_CAPTURE_REFUS = « <champ><chemin>|<champ><chemin>... » (champ : 0 jeu, 1 mise à jour) : une capture par cause
        {
            std::string l = env("WAKFURECOMP_CAPTURE_REFUS").empty() ? std::string() : u8(env("WAKFURECOMP_CAPTURE_REFUS"));
            int n = 0;
            for (size_t a = 0; a < l.size();)
            {
                size_t b = l.find('|', a);
                if (b == std::string::npos) b = l.size();
                if (b > a + 1)
                {
                    int champ = l[a] == '1' ? 1 : 0;
                    fs::path f = fromU8(l.substr(a + 1, b - a - 1));
                    pas.push_back({ fmt("03-refus-%02d", ++n), [this, champ, f] { jeu = maj = {}; refus.clear(); choisirFichier(f, champ); }, aucun });
                }
                a = b + 1;
            }
        }
        pas.push_back({ "04-choix-complet", [this, fJeu, fMaj] { jeu = maj = {}; refus.clear(); choisirFichier(fJeu, -1, true); choisirFichier(fMaj, -1, true); }, aucun });
        pas.push_back({ "04b-choix-mauvaise-version", [this] {   // état simulé : wakfu-prepare, code 3 (SHA-1 de l'image)
            jeu.ok = false;
            jeu.erreur = tr(T_ErrVersionImage);
            refus = std::string(tr(T_MauvaiseCopieDetail)) + "\n" + fmt(tr(T_ShaImage), "0123456789abcdef0123456789abcdef01234567");
        }, aucun });
        pas.push_back({ "04c-choix-complet", [this, fJeu] { refus.clear(); choisirFichier(fJeu, 0); }, aucun });
        if (reel)
        {
            pas.push_back({ "05-verification", [this] { verifier(); }, [this] { return job == Job::Aucun; } });
            pas.push_back({ "06-verification-fin", [] {}, aucun });
            pas.push_back({ "07-progression", [] {}, [this] { return job == Job::Aucun; } });   // construction lancée par la fin de la vérification
            pas.push_back({ "08-fin-recompilation", [] {}, aucun });
        }
        else
        {
            pas.push_back({ "05-verification-en-cours", [this] { ecran = Ecran::Verification; job = Job::Verification; gb = 41.7; faux(EtOutils, 0.37, 12); }, aucun });
            pas.push_back({ "07-verification-echec", [this] {
                job = Job::Aucun;
                faux(EtPreparation, -1, 0);
                std::lock_guard l(g_t.m);
                g_t.echec = true;
                g_t.erreur = "préparation du jeu : échec (code 1)";
                g_t.fin = { "jeu : paquet STFS (jeu, titre 584109AA), Z:/jeux/B3B621777223B7687094B7D47E4F67C4B5E861A558",
                            "mise à jour : paquet STFS (mise à jour, titre 584109AA), Z:/jeux/TU_1C422DA_0000004000000.00000000000G1",
                            "erreur : Z:/jeux/B3B6... : lecture impossible à l'offset 859410432 (fichier tronqué ?)" };
            }, aucun });
            pas.push_back({ "08-verification-deja-preparee", [this] { faux(EtPreparation, -1, 0); { std::lock_guard l(g_t.m); g_t.fin.clear(); } verifOk = true; dejaPrepare = true; }, aucun });
            pas.push_back({ "09-progression", [this] { ecran = Ecran::Progression; job = Job::Construction; faux(EtCompilation, 0.42, 560, "ppc_recomp.37.cpp.obj"); }, aucun });
            pas.push_back({ "10-progression-xenon", [this] { faux(EtXenon, 0.27, 30); }, aucun });
            pas.push_back({ "11-progression-echec", [this] {
                job = Job::Aucun;
                faux(EtCompilation, 0.42, 0);
                std::lock_guard l(g_t.m);
                g_t.echec = true;
                g_t.erreur = "compilation : échec (code 1)";
                g_t.fin = { "[1234/2950] Building CXX object ppc/CMakeFiles/ppc.dir/ppc_recomp.37.cpp.obj",
                            "FAILED: ppc/CMakeFiles/ppc.dir/ppc_recomp.37.cpp.obj",
                            "clang++: error: unable to execute command: Couldn't allocate memory",
                            "ninja: build stopped: subcommand failed." };
            }, aucun });
            pas.push_back({ "12-reglages-fin", [this] { faux(EtInstallation, -1, 0); chargerImages(); finRecompilation = true; ecran = Ecran::Reglages; ongletForce = 0; }, aucun });
        }
        pas.push_back({ "13-reglages-affichage", [this] { chargerImages(); finRecompilation = false; ecran = Ecran::Reglages; ongletForce = 0; }, aucun });
        pas.push_back({ "14-reglages-image", [this] { ongletForce = 1; }, aucun });
        pas.push_back({ "15-reglages-son-manette", [this] { ongletForce = 2; }, aucun });
        pas.push_back({ "16-reglages-clavier", [this] { ongletForce = 3; c.keys["a"] = "Space"; attenteTouche = 5; }, aucun });
        pas.push_back({ "17-jouer", [this] { ongletForce = -1; attenteTouche = -1; c.keys.clear(); ecran = Ecran::Jouer; }, aucun });
        pas.push_back({ "18-jouer-plantage", [this] { codeJeu = 6; finJeu(); }, aucun });
        pas.push_back({ "19-confirmer-recompilation", [this] { messageJeu.clear(); confirmerRecompilation = true; }, aucun });
        pas.push_back({ "21-verification-cases", [this] {   // état simulé : vérification réussie (cases ménage / bureau)
            confirmerRecompilation = false; ecran = Ecran::Verification; job = Job::Aucun;
            faux(EtPreparation, -1, 0); { std::lock_guard l(g_t.m); g_t.fin.clear(); } verifOk = true; dejaPrepare = false; gb = 41.7;
        }, aucun });
        pas.push_back({ "22-anciennes", [this] {   // état simulé si aucune ancienne installation réelle
            if (anciennes.empty()) anciennes = { { "/exemple/wakfu-recomp-natif", {}, 3u << 30 } };
            ecran = Ecran::Anciennes;
        }, aucun });
        pas.push_back({ "23-desinstaller", [this] { anciennes.clear(); preparerDesinstallation(); }, aucun });
        pas.push_back({ "24-desinstaller-sauvegardes", [this] { confirmerSauvegardes = true; }, aucun });
        capturesFinales(reel);
        dernierePeriodique = Clock::now();
    }

    void capturesFinales(bool reel)
    {
        auto aucun = [] { return true; };
        if (reel && std::getenv("WAKFURECOMP_CAPTURE_JOUER"))   // jeu lancé pour de vrai (WAKFU_STOP_AT_SWAP pour l'arrêter)
            pas.push_back({ "25-jeu", [this] { confirmerSauvegardes = false; ecran = Ecran::Jouer; jouer(); },
                            [this] { return !jeuEnCours && !(SDL_GetWindowFlags(win) & SDL_WINDOW_HIDDEN); } });
        if (reel && std::getenv("WAKFURECOMP_CAPTURE_DESINSTALLER"))   // pour de vrai ; ..._EFFACER_SAUVEGARDES : sauvegardes comprises
        {
            pas.push_back({ "26-desinstallation", [this] {
                confirmerSauvegardes = false;
                preparerDesinstallation();
                if (std::getenv("WAKFURECOMP_CAPTURE_EFFACER_SAUVEGARDES")) { effacerSauvegardes = true; listerDesinstallation(); }
                lancerDesinstallation();
            }, [this] { return job == Job::Aucun; } });
            pas.push_back({ "27-desinstalle", [] {}, aucun });
        }
        else if (!reel) pas.push_back({ "26-desinstalle", [this] { confirmerSauvegardes = false; ecran = Ecran::Desinstalle; }, aucun });
    }

    void avancerCapture()   // après le rendu, avant la présentation
    {
        if (pasCourant >= pas.size()) { quitter = true; return; }
        Pas& p = pas[pasCourant];
        if (images == 0) p.prep();
        ++images;
        if (images < 4) return;                // mise en page stabilisée (tailles calculées au cadre précédent)
        if (!p.attendre())
        {
            if (Clock::now() - dernierePeriodique > std::chrono::seconds(90))   // capture périodique pendant l'attente
            {
                dernierePeriodique = Clock::now();
                enregistrerCapture(fmt("%s-%03d", p.nom.c_str(), ++periodique));
            }
            return;
        }
        enregistrerCapture(p.nom);
        ++pasCourant, images = 0;
    }

    int boucle()
    {
        const char* cap = std::getenv("WAKFURECOMP_CAPTURE");
        if (cap) preparerCapture(fromU8(cap));
        while (!quitter)
        {
            SDL_Event ev;
            bool attendre = jeuEnCours || (SDL_GetWindowFlags(win) & SDL_WINDOW_MINIMIZED);
            if (attendre ? SDL_WaitEventTimeout(&ev, 100) : SDL_PollEvent(&ev))
                do
                {
                    if (ev.type == SDL_KEYDOWN && attenteTouche >= 0 && ecran == Ecran::Reglages)   // touche choisie pour une commande
                    {
                        const char* n = SDL_GetScancodeName(ev.key.keysym.scancode);
                        if (n && *n) c.keys[kCommandes[attenteTouche].nom] = n;
                        attenteTouche = -1;
                        continue;
                    }
                    ImGui_ImplSDL2_ProcessEvent(&ev);
                    if (ev.type == SDL_DROPFILE)
                    {
                        if (ecran == Ecran::Choix) deposer(fromU8(ev.drop.file));
                        SDL_free(ev.drop.file);
                    }
                    if (ev.type == SDL_QUIT || (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE))
                    {
                        if (job != Job::Aucun) confirmerQuitter = true;
                        else quitter = true;
                    }
                } while (SDL_PollEvent(&ev));
            bool fini;
            { std::lock_guard l(g_t.m); fini = g_t.fini; }
            if (fini && job != Job::Aucun) finTravail();
            if (attendre)
            {
                if (!jeuEnCours && (SDL_GetWindowFlags(win) & SDL_WINDOW_HIDDEN)) finJeu();
                continue;
            }
            ImGui_ImplSDLRenderer2_NewFrame();
            ImGui_ImplSDL2_NewFrame();
            ImGui::NewFrame();
            image();
            ImGui::Render();
            SDL_RenderSetScale(ren, ImGui::GetIO().DisplayFramebufferScale.x, ImGui::GetIO().DisplayFramebufferScale.y);
            SDL_SetRenderDrawColor(ren, 0x12, 0x0B, 0x07, 255);
            SDL_RenderClear(ren);
            ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ren);
            if (cap) avancerCapture();
            SDL_RenderPresent(ren);
        }
        if (job != Job::Aucun || jeuEnCours) quitterDeSuite();   // fil de travail bloqué sur un outil : arrêté avec nous
        if (fil.joinable()) fil.join();
        return 0;
    }
};

void appliquerTheme()
{
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 16, st.ChildRounding = 18, st.FrameRounding = 10, st.PopupRounding = 14, st.GrabRounding = 8;
    st.TabRounding = 10, st.ScrollbarRounding = 8, st.ScrollbarSize = 12, st.GrabMinSize = 14;
    st.FramePadding = ImVec2(16, 10), st.ItemSpacing = ImVec2(12, 10), st.ItemInnerSpacing = ImVec2(10, 8);
    st.CellPadding = ImVec2(8, 9), st.WindowBorderSize = 0, st.ChildBorderSize = 1, st.PopupBorderSize = 1, st.FrameBorderSize = 0;
    st.TabBarBorderSize = 1;
    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = rgb(kTexte);
    c[ImGuiCol_TextDisabled] = rgb(kTexteDoux, 0.7f);
    c[ImGuiCol_WindowBg] = rgb(kCarte);
    c[ImGuiCol_PopupBg] = rgb(kCarte, 0.98f);
    c[ImGuiCol_ModalWindowDimBg] = rgb(0x0A0503, 0.65f);
    c[ImGuiCol_Border] = rgb(kBord, 0.6f);
    c[ImGuiCol_FrameBg] = rgb(0x3A281B);
    c[ImGuiCol_FrameBgHovered] = rgb(0x4A3322);
    c[ImGuiCol_FrameBgActive] = rgb(0x573B26);
    c[ImGuiCol_CheckMark] = rgb(kOrange);
    c[ImGuiCol_SliderGrab] = rgb(kOrange);
    c[ImGuiCol_SliderGrabActive] = rgb(kOrangeClair);
    c[ImGuiCol_Button] = rgb(kSecondaire);
    c[ImGuiCol_ButtonHovered] = rgb(kSecondaireClair);
    c[ImGuiCol_ButtonActive] = rgb(kOrangeFonce);
    c[ImGuiCol_Header] = rgb(kOrange, 0.30f);
    c[ImGuiCol_HeaderHovered] = rgb(kOrange, 0.45f);
    c[ImGuiCol_HeaderActive] = rgb(kOrange, 0.60f);
    c[ImGuiCol_Tab] = rgb(kSecondaire, 0.0f);
    c[ImGuiCol_TabHovered] = rgb(kSecondaireClair);
    c[ImGuiCol_TabSelected] = rgb(kOrange, 0.22f);
    c[ImGuiCol_TabSelectedOverline] = rgb(kOrange);
    c[ImGuiCol_TabDimmed] = rgb(kSecondaire, 0.0f);
    c[ImGuiCol_TabDimmedSelected] = rgb(kOrange, 0.22f);
    c[ImGuiCol_Separator] = rgb(kBord, 0.6f);
    c[ImGuiCol_ScrollbarBg] = rgb(0, 0);
    c[ImGuiCol_ScrollbarGrab] = rgb(kBord);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(kOrangeFonce);
    c[ImGuiCol_ScrollbarGrabActive] = rgb(kOrange);
    c[ImGuiCol_TableRowBg] = rgb(0, 0);
    c[ImGuiCol_TableRowBgAlt] = rgb(0, 0);
    c[ImGuiCol_NavCursor] = rgb(kOrangeClair);
    c[ImGuiCol_TextSelectedBg] = rgb(kOrange, 0.35f);
    st.ScaleAllSizes(g_s);
    st.FontScaleDpi = g_s;
    st.FontSizeBase = kTexteTaille;
}

int erreurBoite(const std::string& m)
{
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "WakfuRecomp", (std::string(tr(T_ErreurDemarrage)) + "\n\n" + m).c_str(), nullptr);
    return 1;
}

} // namespace

int guiMain()
{
    initLangue();
    g_suivi = &g_suiviFenetre;
#ifdef _WIN32
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
#endif
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) return erreurBoite(std::string("SDL_Init : ") + SDL_GetError());
    App app;
    try
    {
        g_s = std::clamp(ImGui_ImplSDL2_GetContentScaleForDisplay(0), 1.0f, 3.0f);
        SDL_Rect ub;   // la fenêtre doit tenir dans l'écran (barre des tâches comprise)
        if (SDL_GetDisplayUsableBounds(0, &ub) == 0 && ub.w > 0 && ub.h > 0)
            g_s = std::max(1.0f, std::min({ g_s, ub.w / 1160.0f, ub.h / 740.0f }));
        if (const char* e = std::getenv("WAKFURECOMP_ECHELLE")) g_s = std::clamp(float(std::atof(e)), 1.0f, 3.0f);
        app.win = SDL_CreateWindow(tr(T_TitreFenetre), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, int(1120 * g_s), int(700 * g_s),
                                   SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_SHOWN);
        if (!app.win) return erreurBoite(std::string("SDL_CreateWindow : ") + SDL_GetError());
        app.ren = SDL_CreateRenderer(app.win, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);
        if (!app.ren) app.ren = SDL_CreateRenderer(app.win, -1, SDL_RENDERER_SOFTWARE);   // sans pilote graphique utilisable
        if (!app.ren) return erreurBoite(std::string("SDL_CreateRenderer : ") + SDL_GetError());
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
#ifdef _WIN32
        g_regular = policeRessource(L"INTER_REGULAR");
        g_semibold = policeRessource(L"INTER_SEMIBOLD");
#else
        g_regular = policeRessource(kInterRegular, sizeof kInterRegular);
        g_semibold = policeRessource(kInterSemiBold, sizeof kInterSemiBold);
#endif
        appliquerTheme();
        ImGui_ImplSDL2_InitForSDLRenderer(app.win, app.ren);
        ImGui_ImplSDLRenderer2_Init(app.ren);
        app.demarrer();
        app.boucle();
    }
    catch (const std::exception& e)
    {
        journal(std::string("ERREUR : ") + e.what());
        erreurBoite(e.what());
        if (app.job != Job::Aucun) quitterDeSuite();
        if (app.fil.joinable()) app.fil.join();
        return 1;
    }
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(app.ren);
    SDL_DestroyWindow(app.win);
    SDL_Quit();
    if (g_t.journal) std::fclose(g_t.journal);
    return 0;
}

} // namespace wk
