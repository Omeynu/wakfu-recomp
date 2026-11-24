// XAM et pilote audio, version hors ligne : profil local signé, manette 0 branchée au repos,
// aucune notification ; pilote audio : trames mixées vers la sortie son de l'hôte (audio_out).
#include <cstdio>
#include "../quit.h"
#include <algorithm>
#include <cmath>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "../process.h"
#include "audio_out.h"
#include "profile.h"
#include "stub.h"
#include "sync.h"
#include "window.h"
#include "xma.h"

using namespace wk;

namespace {
constexpr uint32_t ERROR_SUCCESS = 0, ERROR_DEVICE_NOT_CONNECTED = 1167, ERROR_EMPTY = 4306,
                   ERROR_NO_SUCH_USER = 1317;
constexpr uint64_t kXuid = 0xE000000000000001ull;   // XUID local (hors ligne)
void StoreFloat(uint32_t a, float f) { uint32_t b; memcpy(&b, &f, 4); St32(a, b); }
}  // namespace

// ---------- Système ----------
WK_IMPORT(XamGetExecutionId) {
    WK_TRACE(XamGetExecutionId);
    // *r3 = adresse de X_EXECUTION_ID (données de l'en-tête EXECUTION_INFO)
    bool found = false;
    const uint32_t info = XexHeaderField(0x00040006, &found);
    St32(ctx.r3.u32, found ? info : 0);
    Ret(ctx, found ? STATUS_SUCCESS : STATUS_NOT_FOUND);
}
WK_IMPORT(XamLoaderTerminateTitle) {
    WK_TRACE(XamLoaderTerminateTitle);
    wk::Quit(wk::kExitOk, "le jeu demande à quitter (XamLoaderTerminateTitle)");
}
WK_IMPORT(NetDll_WSAStartup) {
    WK_TRACE(NetDll_WSAStartup);
    // (appelant, version, WSADATA*) : succès, réseau hors ligne ensuite
    if (ctx.r5.u32) { St16(ctx.r5.u32, uint16_t(ctx.r4.u32)); St16(ctx.r5.u32 + 2, 0x0202); }
    Ret(ctx, 0);
}

// ---------- Notifications ----------
WK_IMPORT(XamNotifyCreateListener) {
    WK_TRACE(XamNotifyCreateListener);
    Ret(ctx, NewHandle(HandleKind::Listener, 0));
}
WK_IMPORT(XNotifyGetNext) { WK_TRACE(XNotifyGetNext); Ret(ctx, 0); }       // aucune notification
WK_IMPORT(XNotifyPositionUI) { WK_TRACE(XNotifyPositionUI); }

// ---------- Utilisateurs ----------
// Joueur 2 (utilisateur 1) connecté, profil local « Joueur 2 », tant que ses commandes sont branchées (coopératif)
namespace { bool Connected(uint32_t user); }
WK_IMPORT(XamUserGetSigninState) { WK_TRACE(XamUserGetSigninState); Ret(ctx, Connected(ctx.r3.u32) ? 1 : 0); }  // 1 = local
WK_IMPORT(XamUserGetXUID) {
    WK_TRACE(XamUserGetXUID);
    // (utilisateur, masque, XUID*)
    if (!Connected(ctx.r3.u32)) { Ret(ctx, ERROR_NO_SUCH_USER); return; }
    St64(ctx.r5.u32, kXuid + ctx.r3.u32);
    Ret(ctx, ERROR_SUCCESS);
}
WK_IMPORT(XamUserGetSigninInfo) {
    WK_TRACE(XamUserGetSigninInfo);
    // (utilisateur, drapeaux, X_USER_SIGNIN_INFO*) : xuid, ?, état, ?, ?, nom[16]
    const uint32_t out = ctx.r5.u32;
    for (uint32_t i = 0; i < 0x28; i += 4) St32(out + i, 0);
    if (!Connected(ctx.r3.u32)) { Ret(ctx, ERROR_NO_SUCH_USER); return; }
    St64(out, kXuid + ctx.r3.u32);
    St32(out + 0x0C, 1);
    memcpy(Host(out + 0x18), ctx.r3.u32 ? "Joueur 2" : "Joueur", ctx.r3.u32 ? 8 : 6);
    Ret(ctx, ERROR_SUCCESS);
}

// ---------- Entrées : joueur 1 toujours branché, joueur 2 selon [entrees] joueur2 (script, manette SDL ou clavier) ----------
WK_IMPORT(XamInputGetCapabilities) {
    WK_TRACE(XamInputGetCapabilities);
    // (utilisateur, drapeaux, X_INPUT_CAPABILITIES*) : type, sous-type, drapeaux, manette (12), vibration (4)
    if (!Connected(ctx.r3.u32)) { Ret(ctx, ERROR_DEVICE_NOT_CONNECTED); return; }
    const uint32_t c = ctx.r5.u32;
    for (uint32_t i = 0; i < 0x14; i += 4) St32(c + i, 0);
    St8(c, 1); St8(c + 1, 1);
    St16(c + 4, 0xF3FF);                          // boutons présents
    St8(c + 6, 0xFF); St8(c + 7, 0xFF);           // gâchettes
    St16(c + 8, 0xFFC0); St16(c + 10, 0xFFC0); St16(c + 12, 0xFFC0); St16(c + 14, 0xFFC0);
    St16(c + 16, 0xFFFF); St16(c + 18, 0xFFFF);   // vibrations
    Ret(ctx, ERROR_SUCCESS);
}
// Entrées scriptées pour les tests : WAKFU_INPUT="3:START,6:A,9:LUP/2.5" (secondes depuis le démarrage :
// bouton maintenu 150 ms, ou la durée donnée après « / »). Boutons : UP DOWN LEFT RIGHT START BACK LB RB A B X Y ;
// stick gauche poussé à fond : LUP LDOWN LLEFT LRIGHT. WAKFU_INPUT2 : même forme pour le joueur 2 (branché s'il est défini).
namespace {
struct Press { double t, d; uint32_t mask; };
constexpr uint32_t kLUp = 1u << 16, kLDown = 1u << 17, kLLeft = 1u << 18, kLRight = 1u << 19;
std::vector<Press> LoadScript(const char* var) {
    std::vector<Press> out;
    const char* e = getenv(var);
    if (!e) return out;
    static const struct { const char* n; uint32_t m; } kNames[] = {
        {"UP", 0x1}, {"DOWN", 0x2}, {"LEFT", 0x4}, {"RIGHT", 0x8}, {"START", 0x10}, {"BACK", 0x20},
        {"LB", 0x100}, {"RB", 0x200}, {"A", 0x1000}, {"B", 0x2000}, {"X", 0x4000}, {"Y", 0x8000},
        {"LUP", kLUp}, {"LDOWN", kLDown}, {"LLEFT", kLLeft}, {"LRIGHT", kLRight}};
    std::string s = e;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find(',', i);
        if (j == std::string::npos) j = s.size();
        std::string item = s.substr(i, j - i);
        size_t c = item.find(':');
        if (c != std::string::npos) {
            std::string name = item.substr(c + 1);
            double d = 0.15;
            if (size_t k = name.find('/'); k != std::string::npos) { d = atof(name.c_str() + k + 1); name.resize(k); }
            for (auto& k : kNames)
                if (name == k.n) out.push_back({atof(item.substr(0, c).c_str()), d, k.m});
        }
        i = j + 1;
    }
    return out;
}
const std::vector<Press> g_scripts[2] = {LoadScript("WAKFU_INPUT"), LoadScript("WAKFU_INPUT2")};
const bool g_script2 = getenv("WAKFU_INPUT2") && *getenv("WAKFU_INPUT2");
const auto g_start = std::chrono::steady_clock::now();
window::Pad ScriptedPad(int joueur) {
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    uint32_t m = 0;
    for (auto& p : g_scripts[joueur])
        if (t >= p.t && t < p.t + p.d) m |= p.mask;
    window::Pad pad;
    pad.buttons = uint16_t(m);
    pad.ly = (m & kLUp) ? 32767 : (m & kLDown) ? -32768 : 0;
    pad.lx = (m & kLRight) ? 32767 : (m & kLLeft) ? -32768 : 0;
    return pad;
}
// Joueurs 1 et 2 (utilisateurs 0 et 1) ; le joueur 1 est toujours branché.
bool Connected(uint32_t user) { return user == 0 || (user == 1 && (g_script2 || window::PadPresent(1))); }
// Script d'entrées prioritaire pendant ses appuis, sinon manette SDL / clavier de la fenêtre.
window::Pad CurrentPad(int joueur) {
    if (window::Pad p = ScriptedPad(joueur); !(p == window::Pad{})) return p;
    return window::PadState(joueur);
}
// Boutons + directions des sticks et gâchettes (pour les frappes, comme XInput)
uint32_t KeystrokeMask(const window::Pad& p) {
    constexpr int kThumb = 16384, kTrigger = 30;
    uint32_t m = p.buttons;
    if (p.lt > kTrigger) m |= 1u << 16;
    if (p.rt > kTrigger) m |= 1u << 17;
    if (p.ly > kThumb) m |= 1u << 18;
    if (p.ly < -kThumb) m |= 1u << 19;
    if (p.lx > kThumb) m |= 1u << 20;
    if (p.lx < -kThumb) m |= 1u << 21;
    if (p.ry > kThumb) m |= 1u << 22;
    if (p.ry < -kThumb) m |= 1u << 23;
    if (p.rx > kThumb) m |= 1u << 24;
    if (p.rx < -kThumb) m |= 1u << 25;
    return m;
}
}  // namespace

WK_IMPORT(XamInputGetState) {
    WK_TRACE(XamInputGetState);
    // (utilisateur, drapeaux, X_INPUT_STATE*) : numéro de paquet, boutons, gâchettes, sticks
    const uint32_t u = ctx.r3.u32;
    if (!Connected(u)) { Ret(ctx, ERROR_DEVICE_NOT_CONNECTED); return; }
    static std::mutex m;
    static window::Pad last[2];
    static uint32_t packet[2] = {};
    const window::Pad p = CurrentPad(u);
    uint32_t number;
    {
        std::lock_guard<std::mutex> lock(m);
        if (!(p == last[u])) {
            packet[u]++;
            if (p.buttons && p.buttons != last[u].buttons) fprintf(stderr, "[entrée] joueur %u boutons %04X\n", u + 1, p.buttons);
            last[u] = p;
        }
        number = packet[u];
    }
    const uint32_t s = ctx.r5.u32;
    St32(s, number);
    St16(s + 4, p.buttons);
    St8(s + 6, p.lt); St8(s + 7, p.rt);
    St16(s + 8, uint16_t(p.lx)); St16(s + 10, uint16_t(p.ly)); St16(s + 12, uint16_t(p.rx)); St16(s + 14, uint16_t(p.ry));
    Ret(ctx, ERROR_SUCCESS);
}
// (utilisateur, inutilisé, XINPUT_VIBRATION* : moteurs gauche et droit sur 16 bits)
WK_IMPORT(XamInputSetState) {
    WK_TRACE(XamInputSetState);
    if (!Connected(ctx.r3.u32)) { Ret(ctx, ERROR_DEVICE_NOT_CONNECTED); return; }
    if (ctx.r5.u32) window::Rumble(int(ctx.r3.u32), Ld16(ctx.r5.u32), Ld16(ctx.r5.u32 + 2));
    Ret(ctx, ERROR_SUCCESS);
}
WK_IMPORT(XamInputGetKeystrokeEx) {
    WK_TRACE(XamInputGetKeystrokeEx);
    // (utilisateur*, drapeaux, X_INPUT_KEYSTROKE*) : frappes générées par le script d'entrées ou la manette
    // X_INPUT_KEYSTROKE : touche virtuelle u16, unicode u16, drapeaux u16 (1 appui, 2 relâché), utilisateur u8, hid u8
    static const struct { uint32_t mask; uint16_t vk; } kVk[] = {
        {0x1000, 0x5800}, {0x2000, 0x5801}, {0x4000, 0x5802}, {0x8000, 0x5803}, {0x200, 0x5804}, {0x100, 0x5805},
        {1u << 16, 0x5806}, {1u << 17, 0x5807},
        {0x1, 0x5810}, {0x2, 0x5811}, {0x4, 0x5812}, {0x8, 0x5813}, {0x10, 0x5814}, {0x20, 0x5815},
        {0x40, 0x5816}, {0x80, 0x5817},
        {1u << 18, 0x5820}, {1u << 19, 0x5821}, {1u << 20, 0x5822}, {1u << 21, 0x5823},
        {1u << 22, 0x5830}, {1u << 23, 0x5831}, {1u << 24, 0x5832}, {1u << 25, 0x5833}};
    // *r3 : utilisateur demandé (0xFF ou drapeau XINPUT_FLAG_ANYUSER 0x40000000 de r4 : n'importe lequel),
    // remplacé par celui de la frappe
    static std::mutex m;
    static std::deque<std::pair<uint16_t, uint16_t>> queue[2];   // par joueur : (touche, drapeaux)
    static uint32_t last[2] = {};
    std::lock_guard<std::mutex> lock(m);
    const uint32_t want = !ctx.r3.u32 || (ctx.r4.u32 & 0x40000000) ? 0xFF : Ld32(ctx.r3.u32);
    for (uint32_t u = 0; u < 2; u++) {
        const uint32_t now = Connected(u) ? KeystrokeMask(CurrentPad(u)) : 0;
        if (now == last[u]) continue;
        for (auto& k : kVk) {
            if ((now & k.mask) && !(last[u] & k.mask)) queue[u].push_back({k.vk, 1});
            if (!(now & k.mask) && (last[u] & k.mask)) queue[u].push_back({k.vk, 2});
        }
        while (queue[u].size() > 32) queue[u].pop_front();   // joueur jamais lu par le jeu
        last[u] = now;
    }
    uint32_t u = want == 0xFF ? (queue[0].empty() ? 1 : 0) : want;
    if (u > 1 || queue[u].empty()) { Ret(ctx, ERROR_EMPTY); return; }
    auto [vk, flags] = queue[u].front();
    queue[u].pop_front();
    const uint32_t out = ctx.r5.u32;
    St16(out, vk); St16(out + 2, 0); St16(out + 4, flags); St8(out + 6, uint8_t(u)); St8(out + 7, 0);
    if (ctx.r3.u32) St32(ctx.r3.u32, u);
    if (flags == 1) fprintf(stderr, "[entrée] joueur %u touche %04X\n", u + 1, vk);
    Ret(ctx, ERROR_SUCCESS);
}

// ---------- Pilote audio ----------
WK_IMPORT(XAudioGetSpeakerConfig) { WK_TRACE(XAudioGetSpeakerConfig); St32(ctx.r3.u32, 0x00010001); Ret(ctx, 0); }
WK_IMPORT(XAudioGetVoiceCategoryVolume) { WK_TRACE(XAudioGetVoiceCategoryVolume); StoreFloat(ctx.r4.u32, 1.0f); Ret(ctx, 0); }
// Pilote audio : un thread invité appelle chaque client toutes les 5,33 ms (256 échantillons à 48 kHz),
// comme le matériel ; les trames rendues par les clients pendant un tour sont additionnées (mixage du matériel) en
// une seule trame de sortie. Avant, elles étaient mises bout à bout : avec 3-4 clients actifs, le son du jeu passait
// à 1/3-1/4 de sa vitesse, haché de trames d'autres clients, et les vidéos (calées sur l'horloge du son) ralentissaient. Avec une sortie son, la file de l'hôte est tenue à kTargetFrames trames (64 ms) : jusqu'à
// 4 tours par réveil quand elle baisse, aucun quand elle est pleine. La cadence suit l'horloge du périphérique et
// la réserve couvre son quantum (1024 échantillons sous PipeWire) et les retards du fil. Avant (une trame par
// réveil), la file restait presque vide : chaque retard de plus de 5 ms ou appel de 1024 échantillons tombant
// avant la 4e trame laissait un trou de silence (son haché).
namespace {
std::mutex g_audioMutex;
// argCell : cellule invitée contenant l'argument ; le rappel reçoit son adresse (il fait objet = *r3).
struct AudioClient { uint32_t callback, arg, argCell; std::chrono::steady_clock::time_point active; };
std::vector<AudioClient> g_audioClients;
constexpr uint32_t kTargetFrames = 12;
std::atomic<uint32_t> g_submits[4];   // trames reçues par client (bilan du journal)
std::mutex g_mixMutex;
float g_mix[2 * audio::kFrameSamples];   // somme des trames du tour (stéréo)
bool g_mixUsed = false;
float g_crete[6] = {};   // crête par canal des trames du jeu (G D C BF AG AD) depuis le dernier bilan
void StartAudioThread() {
    GuestThread* t = CreateGuestThread(0x10000);
    std::thread([t] {
        PPCContext ctx{};
        AttachThread(t, ctx);
        const bool output = audio::Open();
        using clock = std::chrono::steady_clock;
        auto next = clock::now(), bilan = next + std::chrono::seconds(10);
        double retardMax = 0, rappelMax = 0;
        uint32_t fileMin = UINT32_MAX, tours = 0;
        for (;;) {
            next += std::chrono::microseconds(5333);
            std::this_thread::sleep_until(next);
            const auto reveil = clock::now();
            retardMax = std::max(retardMax, std::chrono::duration<double, std::milli>(reveil - next).count());
            uint32_t n = 1;
            if (output) {
                const uint32_t q = audio::QueuedFrames();
                fileMin = std::min(fileMin, q);
                // un tour par réveil ; rattrapage (jusqu'à 4) seulement si la file a baissé sous 8 trames
                n = q >= kTargetFrames ? 0 : q >= 8 ? 1 : std::min(kTargetFrames - q, 4u);
            }
            for (uint32_t i = 0; i < n; i++) {
                std::vector<AudioClient> clients;
                { std::lock_guard<std::mutex> lock(g_audioMutex); clients = g_audioClients; }
                const auto now = clock::now();
                for (auto& c : clients) {
                    // Le client finit de s'initialiser après l'enregistrement : premier rappel différé de 100 ms.
                    if (now < c.active) continue;
                    ctx.r3.u64 = c.argCell;
                    CallGuest(c.callback, ctx);
                }
                {
                    std::lock_guard<std::mutex> lock(g_mixMutex);
                    if (g_mixUsed) audio::Submit(g_mix);
                    std::fill(std::begin(g_mix), std::end(g_mix), 0.0f);
                    g_mixUsed = false;
                }
                rappelMax = std::max(rappelMax, std::chrono::duration<double, std::milli>(clock::now() - now).count());
                tours++;
            }
            if (reveil >= bilan) {   // diagnostic du son haché : une ligne toutes les 10 s
                bilan += std::chrono::seconds(10);
                const audio::Bilan b = audio::Releve();
                const xma::Bilan x = xma::Releve();
                uint32_t s[4];
                for (int i = 0; i < 4; i++) s[i] = g_submits[i].exchange(0);
                float c[6];
                {
                    std::lock_guard<std::mutex> lock(g_mixMutex);
                    std::copy(std::begin(g_crete), std::end(g_crete), c);
                    std::fill(std::begin(g_crete), std::end(g_crete), 0.0f);
                }
                fprintf(stderr, "[son] 10 s : tours %u, trames par client %u/%u/%u/%u ; sortie : appels %u de %u éch., "
                        "sous-alimentations %u (%.0f ms de silence), file min %u trames ; fil : retard max %.1f ms, "
                        "rappel du jeu max %.1f ms ; xma : décodages %u, sortie vide sans entrée %u ; "
                        "crête des 6 canaux du jeu G %.2f D %.2f C %.2f BF %.2f AG %.2f AD %.2f\n",
                        tours, s[0], s[1], s[2], s[3], b.appels, b.echantillonsParAppel, b.sousAlimentations,
                        b.manquants / 48.0, fileMin == UINT32_MAX ? 0 : fileMin, retardMax, rappelMax, x.decodages, x.aVide,
                        c[0], c[1], c[2], c[3], c[4], c[5]);
                retardMax = rappelMax = 0;
                fileMin = UINT32_MAX;
                tours = 0;
            }
        }
    }).detach();
}
}  // namespace
WK_IMPORT(XAudioRegisterRenderDriverClient) {
    WK_TRACE(XAudioRegisterRenderDriverClient);
    // (X_AUDIO_CALLBACK* {fonction, argument}, pilote*)
    std::lock_guard<std::mutex> lock(g_audioMutex);
    if (g_audioClients.empty()) StartAudioThread();
    const uint32_t index = uint32_t(g_audioClients.size());
    const uint32_t cell = Alloc(Heap::Kernel, 4);
    St32(cell, Ld32(ctx.r3.u32 + 4));
    g_audioClients.push_back({Ld32(ctx.r3.u32), Ld32(ctx.r3.u32 + 4), cell,
                              std::chrono::steady_clock::now() + std::chrono::milliseconds(100)});
    fprintf(stderr, "[audio] client %u : rappel %08X argument %08X\n", index, Ld32(ctx.r3.u32), Ld32(ctx.r3.u32 + 4));
    St32(ctx.r4.u32, 0x41550000 | index);
    Ret(ctx, 0);
}
WK_IMPORT(XAudioUnregisterRenderDriverClient) { WK_TRACE(XAudioUnregisterRenderDriverClient); Ret(ctx, 0); }
WK_IMPORT(XAudioSubmitRenderDriverFrame) {
    WK_TRACE(XAudioSubmitRenderDriverFrame);
    // (pilote 0x4155xxxx, trame*) : 6 x 256 flottants gros-boutistes (voir audio_out.h)
    g_submits[ctx.r3.u32 & 3]++;
    if (ctx.r4.u32) {
        float stereo[2 * audio::kFrameSamples];
        audio::DownmixToStereo(Host(ctx.r4.u32), stereo);
        std::lock_guard<std::mutex> lock(g_mixMutex);
        for (int i = 0; i < 2 * audio::kFrameSamples; i++) g_mix[i] += stereo[i];
        for (int i = 0; i < 6 * audio::kFrameSamples; i++)
        {
            const uint32_t b = Ld32(ctx.r4.u32 + 4 * i);
            float f;
            memcpy(&f, &b, 4);
            g_crete[i / audio::kFrameSamples] = std::max(g_crete[i / audio::kFrameSamples], std::fabs(f));
        }
        g_mixUsed = true;
    }
    Ret(ctx, 0);
}
// Contextes XMA : tableau matériel et décodage dans kernel/xma.cpp
WK_IMPORT(XMACreateContext) {
    WK_TRACE(XMACreateContext);
    const uint32_t c = xma::CreateContext();
    St32(ctx.r3.u32, c);
    Ret(ctx, c ? 0 : 0xC0000017);   // STATUS_NO_MEMORY : 320 contextes pris
}
WK_IMPORT(XMAReleaseContext) { WK_TRACE(XMAReleaseContext); xma::ReleaseContext(ctx.r3.u32); Ret(ctx, 0); }
// Bibliothèque XMA du jeu : sub_82214BF0 crée les contextes (registre clear), sub_82215418 les verrouille avant
// lecture (lock), sub_82215588 recopie les contextes et les relance (kick). Les registres sont traités juste après.
PPC_FUNC_IMPL(__imp__sub_82214BF0);
PPC_FUNC(sub_82214BF0) { __imp__sub_82214BF0(ctx, base); xma::ServiceRegisters(); }
PPC_FUNC_IMPL(__imp__sub_82215418);
PPC_FUNC(sub_82215418) { __imp__sub_82215418(ctx, base); xma::ServiceRegisters(); }
PPC_FUNC_IMPL(__imp__sub_82215588);
PPC_FUNC(sub_82215588) { __imp__sub_82215588(ctx, base); xma::ServiceRegisters(); }

// ---------- Contenus, divers ----------
WK_IMPORT(XamContentGetLicenseMask) {
    WK_TRACE(XamContentGetLicenseMask);
    // (masque*, overlapped*) : jeu XBLA complet (pas la version d'essai)
    St32(ctx.r3.u32, 0xFFFFFFFF);
    Ret(ctx, ERROR_SUCCESS);
}
WK_IMPORT(FscSetCacheElementCount) { WK_TRACE(FscSetCacheElementCount); Ret(ctx, 0); }

// ---------- Messagerie XAM (XMsg) ----------
// Applications : 0xFA lecteur de musique, 0xFB XGI (présence, succès, sessions) -> succès hors ligne.
namespace {
void CompleteOverlapped(uint32_t ov, uint32_t result) {
    // XOVERLAPPED : +0 résultat, +4 octets, +0xC événement, +0x18 erreur étendue
    if (!ov) return;
    St32(ov, result);
    St32(ov + 4, 0);
    St32(ov + 0x18, result);
    if (uint32_t ev = Ld32(ov + 0x0C)) {
        HandleEntry e;
        if (LookupHandle(ev, &e)) { std::lock_guard<std::mutex> lock(g_kernelLock); SignalLocked(e.object, 1); }
    }
}
// Lecteur de musique (XMP, application 0xFA) : aucun morceau du joueur, contrôle laissé au jeu.
// Le tampon commence par le client XMP, puis des pointeurs de sortie (disposition reprise de Xenia).
void XmpReply(uint32_t message, uint32_t buf) {
    if (!buf) return;
    auto out = [&](uint32_t off, uint32_t v) { if (uint32_t p = Ld32(buf + off)) St32(p, v); };
    switch (message) {
        case 0x00070009: out(4, 0); break;                          // XMPGetStatus : inactif
        case 0x0007000B: out(4, 0x3F800000); break;                 // XMPGetVolume : 1.0
        case 0x0007001B: out(4, 0); out(8, 0); break;               // XMPGetPlaybackController : jeu, non verrouillé
        case 0x00070029: out(4, 0); out(8, 0); out(12, 0); break;   // XMPGetPlaybackBehavior
        default: break;                                             // « Set… » : rien à rendre
    }
}
uint32_t XMsgResult(uint32_t app, uint32_t message, uint32_t buf = 0) {
    if (app == 0xFA) { XmpReply(message, buf); return 0; }
    if (app == 0xFB) return 0;
    static std::mutex m;
    static std::vector<uint32_t> seen;
    std::lock_guard<std::mutex> lock(m);
    if (std::find(seen.begin(), seen.end(), (app << 24) ^ message) == seen.end()) {
        seen.push_back((app << 24) ^ message);
        fprintf(stderr, "[xmsg] application %02X message %08X non gérée\n", app, message);
    }
    return 0xC0000001;
}
}  // namespace
WK_IMPORT(XMsgStartIORequest) {
    WK_TRACE(XMsgStartIORequest);
    // (application, message, XOVERLAPPED*, tampon, taille)
    const uint32_t r = XMsgResult(ctx.r3.u32, ctx.r4.u32, ctx.r6.u32);
    if (ctx.r5.u32) { CompleteOverlapped(ctx.r5.u32, r); Ret(ctx, 997); return; }   // ERROR_IO_PENDING
    Ret(ctx, r);
}
WK_IMPORT(XMsgStartIORequestEx) {
    WK_TRACE(XMsgStartIORequestEx);
    fprintf(stderr, "[xmsg] StartIORequestEx application %02X message %08X overlapped %08X tampon %08X taille %08X r8 %08X\n",
            ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32);
    const uint32_t r = XMsgResult(ctx.r3.u32, ctx.r4.u32, ctx.r6.u32);
    if (ctx.r5.u32) { CompleteOverlapped(ctx.r5.u32, r); Ret(ctx, 997); return; }
    Ret(ctx, r);
}
WK_IMPORT(XMsgInProcessCall) {
    WK_TRACE(XMsgInProcessCall);
    fprintf(stderr, "[xmsg] InProcessCall application %02X message %08X param1 %08X param2 %08X\n", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
    Ret(ctx, XMsgResult(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32));
}
WK_IMPORT(XMsgCancelIORequest) { WK_TRACE(XMsgCancelIORequest); Ret(ctx, 0); }

// ---------- Stockage, profil, contenus (sauvegardes) ----------
namespace wk {
extern std::string g_dataDir;
void AddSymbolicLink(std::string link, const std::string& target);
void RemoveSymbolicLink(std::string link);
std::string ContentDir(const std::string& name, bool create);
}
namespace {
uint32_t StackArg(PPCContext& ctx, int n) { return Ld32(ctx.r1.u32 + 0x54 + (n - 8) * 8); }
uint32_t FinishXam(uint32_t overlapped, uint32_t result) {
    if (!overlapped) return result;
    CompleteOverlapped(overlapped, result);
    return 997;   // ERROR_IO_PENDING
}
}  // namespace

WK_IMPORT(XamShowDeviceSelectorUI) {
    WK_TRACE(XamShowDeviceSelectorUI);
    // (utilisateur, type de contenu, drapeaux, taille demandée (64 bits), id*, overlapped) : disque dur fictif n° 1
    if (ctx.r7.u32) St32(ctx.r7.u32, 1);
    Ret(ctx, FinishXam(ctx.r8.u32, 0));
}
WK_IMPORT(XamContentGetDeviceState) {
    WK_TRACE(XamContentGetDeviceState);
    Ret(ctx, FinishXam(ctx.r4.u32, ctx.r3.u32 == 1 ? 0 : ERROR_DEVICE_NOT_CONNECTED));
}
WK_IMPORT(XamContentGetDeviceData) {
    WK_TRACE(XamContentGetDeviceData);
    // X_CONTENT_DEVICE_DATA : id, type (1 = disque dur), total, libre (64 bits), nom (28 caractères UTF-16)
    if (ctx.r3.u32 != 1) { Ret(ctx, ERROR_DEVICE_NOT_CONNECTED); return; }
    const uint32_t d = ctx.r4.u32;
    for (uint32_t i = 0; i < 80; i += 4) St32(d + i, 0);
    St32(d, 1); St32(d + 4, 1);
    St64(d + 8, 20ull << 30); St64(d + 16, 10ull << 30);
    const char* n = "Disque dur";
    for (int i = 0; n[i]; i++) St16(d + 24 + 2 * i, uint16_t((unsigned char)n[i]));
    Ret(ctx, 0);
}
WK_IMPORT(XamUserReadProfileSettings) {
    WK_TRACE(XamUserReadProfileSettings);
    // (titre, utilisateur, nb xuid, xuids, nb réglages, ids, taille*, tampon, overlapped) ; réglages enregistrés par
    // XamUserWriteProfileSettings relus de <données>/profile, les autres « non définis » (valeurs par défaut du jeu).
    const uint32_t count = ctx.r7.u32, ids = ctx.r8.u32, sizePtr = ctx.r9.u32, buf = ctx.r10.u32, ov = StackArg(ctx, 8);
    if (count < 1 || count > 32 || !sizePtr) { Ret(ctx, FinishXam(ov, 87)); return; }   // ERROR_INVALID_PARAMETER
    const uint32_t need = profile::ReadSize(count, ids), have = Ld32(sizePtr);
    if (!buf || have < need) { if (!have) St32(sizePtr, need); Ret(ctx, 122); return; }   // ERROR_INSUFFICIENT_BUFFER
    if (!ctx.r6.u32 && !Connected(ctx.r4.u32)) { Ret(ctx, FinishXam(ov, ERROR_NO_SUCH_USER)); return; }
    // joueur 2 : réglages à part (<données>/profile2), par numéro ou par son XUID
    const uint32_t user = ctx.r6.u32 ? uint32_t(Ld64(ctx.r6.u32) == kXuid + 1) : ctx.r4.u32;
    profile::Read(g_dataDir + (user ? "/profile2" : "/profile"), count, ids, buf, ctx.r4.u32, ctx.r6.u32 ? kXuid + user : 0);
    uint32_t known = 0;
    for (uint32_t i = 0; i < count; i++) known += Ld32(buf + 8 + 40 * i) != 0;
    fprintf(stderr, "[profil] lecture de %u réglage(s) (%u enregistré(s)), premier %08X\n", count, known, Ld32(ids));
    Ret(ctx, FinishXam(ov, 0));
}
WK_IMPORT(XamUserWriteProfileSettings) {
    WK_TRACE(XamUserWriteProfileSettings);
    // (titre, utilisateur, nb réglages, réglages, overlapped)
    const uint32_t count = ctx.r5.u32, settings = ctx.r6.u32, ov = ctx.r7.u32;
    if (!Connected(ctx.r4.u32)) { Ret(ctx, FinishXam(ov, ERROR_NO_SUCH_USER)); return; }
    if (count > 32 || (count && !settings)) { Ret(ctx, FinishXam(ov, 87)); return; }
    const uint32_t n = profile::Write(g_dataDir + (ctx.r4.u32 ? "/profile2" : "/profile"), count, settings);
    fprintf(stderr, "[profil] écriture de %u/%u réglage(s), premier %08X\n", n, count, count ? Ld32(settings + 0x10) : 0);
    Ret(ctx, FinishXam(ov, n == count ? 0 : 0x1F));   // ERROR_GEN_FAILURE
}
WK_IMPORT(XamContentCreateEx) {
    WK_TRACE(XamContentCreateEx);
    // (utilisateur, racine, XCONTENT_DATA*, drapeaux, disposition*, licence*, cache, taille (64 bits), overlapped)
    // XCONTENT_DATA : périphérique, type, nom affiché (128 UTF-16), nom de fichier (42 octets) à +0x108
    const std::string root = ReadAnsi(ctx.r4.u32, 64), name = ReadAnsi(ctx.r5.u32 + 0x108, 42);
    const uint32_t flags = ctx.r6.u32 & 0xF, dispPtr = ctx.r7.u32, licPtr = ctx.r8.u32, ov = StackArg(ctx, 8);
    const bool exists = !ContentDir(name, false).empty();
    uint32_t result = 0, disposition = 0;
    switch (flags) {
        case 1: if (exists) result = 183; else disposition = 1; break;      // CREATE_NEW (ERROR_ALREADY_EXISTS)
        case 2: disposition = 1; break;                                      // CREATE_ALWAYS
        case 3: if (!exists) result = 3; else disposition = 2; break;       // OPEN_EXISTING (ERROR_PATH_NOT_FOUND)
        case 4: disposition = exists ? 2 : 1; break;                         // OPEN_ALWAYS
        case 5: if (!exists) result = 3; else disposition = 1; break;       // TRUNCATE_EXISTING
        default: result = 87;
    }
    if (disposition) {
        ContentDir(name, true);
        AddSymbolicLink(root, "content:\\" + name);
    }
    fprintf(stderr, "[contenu] %s: -> %s, drapeaux %u : %s\n", root.c_str(), name.c_str(), flags,
            result ? "absent ou refusé" : disposition == 1 ? "créé" : "ouvert");
    if (dispPtr) St32(dispPtr, disposition);
    if (licPtr && !result) St32(licPtr, 0);
    Ret(ctx, FinishXam(ov, result));
}
WK_IMPORT(XamContentClose) {
    WK_TRACE(XamContentClose);
    RemoveSymbolicLink(ReadAnsi(ctx.r3.u32, 64));
    Ret(ctx, FinishXam(ctx.r4.u32, 0));
}
WK_IMPORT(XamEnumerate) {
    WK_TRACE(XamEnumerate);
    // (handle, drapeaux, tampon, taille, éléments*, overlapped) : énumération vide
    if (ctx.r7.u32) St32(ctx.r7.u32, 0);
    Ret(ctx, FinishXam(ctx.r8.u32, 18));   // ERROR_NO_MORE_FILES
}
WK_IMPORT(XamShowSigninUI) { WK_TRACE(XamShowSigninUI); Ret(ctx, 0); }
WK_IMPORT(XamShowMessageBoxUIEx) {
    WK_TRACE(XamShowMessageBoxUIEx);
    // (utilisateur, titre, texte, nb boutons, boutons, bouton par défaut, style, résultat*, overlapped)
    const uint32_t result = StackArg(ctx, 8), ov = StackArg(ctx, 9);
    fprintf(stderr, "[xam] boîte de message : %s\n", ReadWide(ctx.r5.u32, 200).c_str());
    if (result) St32(result, ctx.r8.u32);   // bouton par défaut
    Ret(ctx, FinishXam(ov, 0));
}
