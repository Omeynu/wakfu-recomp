// Fenêtre et entrées de l'hôte (voir window.h).
#include "../config.h"
#include <chrono>
#include <thread>
#include "window.h"
#include "../quit.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#ifdef WAKFU_HAS_SDL2
#include <SDL.h>
#ifdef WAKFU_HAS_SHADERGEN
#include <SDL_vulkan.h>
#include "../gpu/present.h"
#include "../natif/device.h"
#endif
#endif

namespace wk { extern std::string g_dataDir; }   // kernel/files.cpp
namespace wk::window {

bool Wanted() {
    if (const char* w = getenv("WAKFU_WINDOW")) return *w == '1';
    for (const char* v : {"WAKFU_CAPTURE", "WAKFU_STOP_AT_SWAP", "WAKFU_STOP_AT_GPU", "WAKFU_INPUT"})
        if (getenv(v)) return false;
    return true;
}

namespace {
std::mutex g_padMutex;
Pad g_pads[2];
bool g_present2 = false;   // joueur 2 branché
std::atomic<bool> g_focus{true}, g_capture{false};
}  // namespace

bool Focused() { return g_focus; }
bool CaptureDemandee() { return g_capture.exchange(false); }
void CaptureFaite() {
#if defined(WAKFU_HAS_SDL2) && defined(WAKFU_HAS_SHADERGEN)
    gpu::AnnoncerCapture();
#endif
}

Pad PadState(int joueur) {
    std::lock_guard<std::mutex> lock(g_padMutex);
    return g_pads[joueur & 1];
}
bool PadPresent(int joueur) {
    std::lock_guard<std::mutex> lock(g_padMutex);
    return joueur == 0 || (joueur == 1 && g_present2);
}

#ifdef WAKFU_HAS_SDL2
namespace {
SDL_Window* g_window = nullptr;
// Manettes 1 à 4 : place libre au branchement, gardée jusqu'au débranchement (attribution stable)
SDL_GameController* g_controllers[4] = {};
std::mutex g_controllerMutex;   // g_controllers : changé par Pump, utilisé par Rumble (thread du jeu)
bool g_sdl = false;
SDL_Scancode g_toucheCapture = SDL_SCANCODE_UNKNOWN;   // option touche_capture

// Options ecran, position_x, position_y, largeur, hauteur : mémorisées à la fermeture (mode fenêtré, fenêtre ni
// agrandie ni réduite), position relative à l'écran.
void MemoriserFenetre() {
    if (!g_window || config::g.mode != "fenetre" ||
        (SDL_GetWindowFlags(g_window) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED | SDL_WINDOW_FULLSCREEN)))
        return;
    const int d = SDL_GetWindowDisplayIndex(g_window);
    SDL_Rect b;
    if (d < 0 || SDL_GetDisplayBounds(d, &b) != 0) return;
    int x = 0, y = 0, w = 0, h = 0;
    SDL_GetWindowPosition(g_window, &x, &y);
    SDL_GetWindowSize(g_window, &w, &h);
    config::SaveWindow(g_dataDir, d, x - b.x, y - b.y, w, h);
}

// Manette du joueur (option joueur1 / joueur2 : "1" à "4"), nulle pour "clavier" ou manette absente
SDL_GameController* ManetteDu(int joueur) {
    const std::string& j = config::g.joueurs[joueur];
    return j == "clavier" ? nullptr : g_controllers[j[0] - '1'];
}
// Joueur qui reçoit le clavier : celui qui a "clavier", sinon le joueur 1
int JoueurClavier() { return config::g.joueurs[1] == "clavier" ? 1 : 0; }
void JournalAttribution() {
    for (int j = 0; j < 2; j++) {
        const std::string& src = config::g.joueurs[j];
        SDL_GameController* c = ManetteDu(j);
        fprintf(stderr, "[entrées] joueur %d : %s%s%s%s%s\n", j + 1, src == "clavier" ? "clavier" : ("manette " + src).c_str(),
                src == "clavier" ? "" : c ? " (" : " (absente)", c ? SDL_GameControllerName(c) : "", c ? ")" : "",
                src != "clavier" && JoueurClavier() == j ? " + clavier" : "");
    }
}
void OpenController(int device) {
    if (!SDL_IsGameController(device)) return;
    const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(device);
    for (SDL_GameController* c : g_controllers)
        if (c && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c)) == id) return;   // déjà ouverte
    for (int n = 0; n < 4; n++)
        if (!g_controllers[n]) {
            if (!(g_controllers[n] = SDL_GameControllerOpen(device))) return;
            fprintf(stderr, "[manette] %d : %s\n", n + 1, SDL_GameControllerName(g_controllers[n]));
            JournalAttribution();
            return;
        }
}
}  // namespace

void Open() {
    SetKeyBindings(config::g.keys);   // option [touches]
    g_toucheCapture = SDL_GetScancodeFromName(config::g.captureKey.c_str());
    if (g_toucheCapture == SDL_SCANCODE_UNKNOWN)
        fprintf(stderr, "[config] [confort] touche_capture : touche « %s » inconnue (pas de capture)\n", config::g.captureKey.c_str());
    // Option entree_arriere_plan : la manette reste lue quand la fenêtre n'a pas le focus
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, config::g.backgroundInput ? "1" : "0");
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0) g_sdl = true, JournalAttribution();
    else fprintf(stderr, "[manette] SDL : %s (pas de manette)\n", SDL_GetError());
#ifdef WAKFU_HAS_SHADERGEN
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[fenetre] SDL : %s (pas de fenêtre)\n", SDL_GetError());
        return;
    }
    g_sdl = true;
    // Options [video] largeur, hauteur, mode (plein_ecran : changement de mode d'affichage ; sans_bordure : bureau)
    const uint32_t modeFlag = config::g.mode == "plein_ecran" ? SDL_WINDOW_FULLSCREEN :
                              config::g.mode == "sans_bordure" ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0;
    // Options ecran, position_x, position_y : écran absent -> principal ; position hors de l'écran -> centrée
    int ecran = config::g.screen;
    if (ecran >= SDL_GetNumVideoDisplays()) {
        fprintf(stderr, "[config] [video] ecran = %d : écran absent (%d écran(s)), écran 0\n", ecran, SDL_GetNumVideoDisplays());
        ecran = 0;
    }
    int x = int(SDL_WINDOWPOS_CENTERED_DISPLAY(ecran)), y = x;
    SDL_Rect b;
    if (config::g.posX && config::g.posY && config::g.mode == "fenetre" && SDL_GetDisplayBounds(ecran, &b) == 0 &&
        *config::g.posX >= 0 && *config::g.posY >= 0 && *config::g.posX < b.w - 64 && *config::g.posY < b.h - 64)
        x = b.x + *config::g.posX, y = b.y + *config::g.posY;
    g_window = SDL_CreateWindow("Islands of Wakfu", x, y, config::g.width,
                                config::g.height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | modeFlag);
    if (!g_window) { fprintf(stderr, "[fenetre] création : %s (pas de fenêtre)\n", SDL_GetError()); return; }
    fprintf(stderr, "[fenetre] %dx%d, mode %s, vsync %s, limite %d img/s, écran %d\n", config::g.width, config::g.height,
            config::g.mode.c_str(), config::g.vsync ? "oui" : "non", config::g.fpsLimit, ecran);
    unsigned n = 0;
    SDL_Vulkan_GetInstanceExtensions(g_window, &n, nullptr);
    std::vector<const char*> names(n);
    if (!SDL_Vulkan_GetInstanceExtensions(g_window, &n, names.data())) {
        fprintf(stderr, "[fenetre] extensions Vulkan : %s\n", SDL_GetError());
        SDL_DestroyWindow(g_window);
        g_window = nullptr;
        return;
    }
    vk::SetInstanceExtensions(std::vector<std::string>(names.begin(), names.begin() + n));
    fprintf(stderr, "[fenetre] ouverte (pilote vidéo %s)\n", SDL_GetCurrentVideoDriver());
#else
    fprintf(stderr, "[fenetre] runtime compilé sans rendu Vulkan : pas de fenêtre\n");
#endif
}

bool IsOpen() { return g_window != nullptr; }

void Pump() {
    if (!g_sdl) return;
    static auto last = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::milliseconds(4)) return;
    last = now;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            MemoriserFenetre();
            Quit(kExitOk, "fenêtre fermée");
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST || e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
                g_focus = e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED;   // option couper_hors_focus
#ifdef WAKFU_HAS_SHADERGEN
            if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) gpu::PresentResize();
#endif
            break;
        case SDL_KEYDOWN:
            if (!e.key.repeat && e.key.keysym.scancode == g_toucheCapture && g_toucheCapture != SDL_SCANCODE_UNKNOWN) g_capture = true;
            break;
        case SDL_CONTROLLERDEVICEADDED: {   // which : index du périphérique
            std::lock_guard<std::mutex> lock(g_controllerMutex);
            OpenController(e.cdevice.which);
            break;
        }
        case SDL_CONTROLLERDEVICEREMOVED: {   // which : identifiant d'instance
            std::lock_guard<std::mutex> lock(g_controllerMutex);
            for (int n = 0; n < 4; n++)
                if (g_controllers[n] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_controllers[n])) == e.cdevice.which) {
                    fprintf(stderr, "[manette] %d débranchée\n", n + 1);
                    SDL_GameControllerClose(g_controllers[n]);
                    g_controllers[n] = nullptr;
                    JournalAttribution();
                }
            break;
        }
        }
    }
    // Clavier : seulement fenêtre active (SDL remet les touches à zéro à la perte du focus)
    const uint8_t* keys = g_window && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS) ? SDL_GetKeyboardState(nullptr) : nullptr;
    // « Quitter » à la manette : BACK + START maintenus 3 secondes (sur une même manette)
    static std::chrono::steady_clock::time_point quitSince{};
    bool comboTenu = false;
    Pad pads[2];
    bool present2 = false;
    for (SDL_GameController* c : g_controllers)
        comboTenu |= c && SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK) && SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START);
    for (int j = 0; j < 2; j++) {
        int16_t axes[6] = {};
        uint32_t buttons = 0;
        SDL_GameController* c = ManetteDu(j);
        if (c) {
            for (int i = 0; i < 6; i++) axes[i] = SDL_GameControllerGetAxis(c, SDL_GameControllerAxis(i));
            for (int i = 0; i < SDL_CONTROLLER_BUTTON_DPAD_RIGHT + 1; i++)
                if (SDL_GameControllerGetButton(c, SDL_GameControllerButton(i))) buttons |= 1u << i;
        }
        pads[j] = MapPad(axes, buttons, JoueurClavier() == j ? keys : nullptr, config::g.deadZone);
        if (j == 1) present2 = c || (config::g.joueurs[1] == "clavier" && g_window);
    }
    if (!comboTenu) quitSince = {};
    else if (quitSince == std::chrono::steady_clock::time_point{}) quitSince = now;
    else if (now - quitSince >= std::chrono::seconds(3)) { MemoriserFenetre(); Quit(kExitOk, "Quitter à la manette (BACK + START maintenus)"); }
    std::lock_guard<std::mutex> lock(g_padMutex);
    g_pads[0] = pads[0], g_pads[1] = pads[1];
    g_present2 = present2;
}

// Option vibrations (intensité 0 à 1) : moteurs gauche (basse fréquence) et droit (haute fréquence), 0 à 65535.
void Rumble(int joueur, uint16_t left, uint16_t right) {
    std::lock_guard<std::mutex> lock(g_controllerMutex);
    const float k = config::g.vibration;
    SDL_GameController* c = ManetteDu(joueur & 1);
    if (c && k > 0.0f) SDL_GameControllerRumble(c, uint16_t(left * k), uint16_t(right * k), 1000);
}

#ifdef WAKFU_HAS_SHADERGEN
// Présentation prête (contexte, surface, swapchain initialisés au premier appel)
bool PresentReady() {
    if (!g_window) return false;
    static bool ready = false, failed = false;
    if (failed) return false;
    if (!ready) {
        vk::Context* ctx = natif::Contexte();   // celui du rendu natif (même périphérique que les images présentées)
        std::string err;
        if (!ctx) return false;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (!SDL_Vulkan_CreateSurface(g_window, ctx->instance, &surface)) {
            fprintf(stderr, "[fenetre] surface : %s (pas de présentation)\n", SDL_GetError());
            failed = true;
            return false;
        }
        if (!gpu::PresentInit(ctx, surface, &err)) {
            fprintf(stderr, "[fenetre] %s (pas de présentation)\n", err.c_str());
            failed = true;
            return false;
        }
        fprintf(stderr, "[fenetre] présentation sur %s\n", ctx->deviceName.c_str());
        ready = true;
    }
    return true;
}
#endif
void AfterPresent();

// Fil natif : image native (layout GENERAL, travail soumis avant sur la même file) -> fenêtre
void PresentNative(VkImage image, uint32_t w, uint32_t h) {
#ifdef WAKFU_HAS_SHADERGEN
    if (!PresentReady()) return;
    int dw = 0, dh = 0;
    SDL_Vulkan_GetDrawableSize(g_window, &dw, &dh);
    gpu::PresentFrameImage(image, w, h, uint32_t(dw), uint32_t(dh));
    AfterPresent();
#else
    (void)image; (void)w; (void)h;
#endif
}

void AfterPresent() {
#ifdef WAKFU_HAS_SHADERGEN
    // Options limite_images (attente jusqu'à l'échéance de l'image) et afficher_images_par_seconde (titre)
    using clock = std::chrono::steady_clock;
    if (config::g.fpsLimit > 0) {
        static clock::time_point next = clock::now();
        const auto period = std::chrono::nanoseconds(1000000000LL / config::g.fpsLimit);
        next += period;
        const auto now = clock::now();
        if (next > now) std::this_thread::sleep_until(next);
        else if (now - next > period * 4) next = now;   // retard important : pas de rattrapage en rafale
    }
    if (config::g.showFps) {
        static clock::time_point t0 = clock::now();
        static int frames = 0;
        frames++;
        const double s = std::chrono::duration<double>(clock::now() - t0).count();
        if (s >= 1.0) {
            char title[64];
            snprintf(title, sizeof title, "Islands of Wakfu - %.1f img/s", frames / s);
            SDL_SetWindowTitle(g_window, title);
            frames = 0;
            t0 = clock::now();
        }
    }
#endif
}
#else
void Open() { fprintf(stderr, "[fenetre] runtime compilé sans SDL2 : pas de fenêtre ni de manette\n"); }
bool IsOpen() { return false; }
void Pump() {}
void Present(const uint32_t*) {}
void PresentNative(VkImage, uint32_t, uint32_t) {}
void Rumble(int, uint16_t, uint16_t) {}
#endif

}  // namespace wk::window
