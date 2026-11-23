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

namespace wk::window {

bool Wanted() {
    if (const char* w = getenv("WAKFU_WINDOW")) return *w == '1';
    for (const char* v : {"WAKFU_CAPTURE", "WAKFU_STOP_AT_SWAP", "WAKFU_STOP_AT_GPU", "WAKFU_INPUT"})
        if (getenv(v)) return false;
    return true;
}

namespace {
std::mutex g_padMutex;
Pad g_pad;
}  // namespace

Pad PadState() {
    std::lock_guard<std::mutex> lock(g_padMutex);
    return g_pad;
}

#ifdef WAKFU_HAS_SDL2
namespace {
SDL_Window* g_window = nullptr;
SDL_GameController* g_controller = nullptr;
std::mutex g_controllerMutex;   // g_controller : changé par Pump, utilisé par Rumble (thread du jeu)
bool g_sdl = false;

void OpenFirstController() {
    for (int i = 0; i < SDL_NumJoysticks() && !g_controller; i++)
        if (SDL_IsGameController(i) && (g_controller = SDL_GameControllerOpen(i)))
            fprintf(stderr, "[manette] %s\n", SDL_GameControllerName(g_controller));
}
}  // namespace

void Open() {
    SetKeyBindings(config::g.keys);   // option [touches]
    // Option entree_arriere_plan : la manette reste lue quand la fenêtre n'a pas le focus
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, config::g.backgroundInput ? "1" : "0");
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0) g_sdl = true;
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
    g_window = SDL_CreateWindow("Islands of Wakfu", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, config::g.width,
                                config::g.height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | modeFlag);
    if (!g_window) { fprintf(stderr, "[fenetre] création : %s (pas de fenêtre)\n", SDL_GetError()); return; }
    fprintf(stderr, "[fenetre] %dx%d, mode %s, vsync %s, limite %d img/s\n", config::g.width, config::g.height,
            config::g.mode.c_str(), config::g.vsync ? "oui" : "non", config::g.fpsLimit);
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
            Quit(kExitOk, "fenêtre fermée");
        case SDL_WINDOWEVENT:
#ifdef WAKFU_HAS_SHADERGEN
            if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) gpu::PresentResize();
#endif
            break;
        case SDL_CONTROLLERDEVICEADDED: {
            std::lock_guard<std::mutex> lock(g_controllerMutex);
            if (!g_controller) OpenFirstController();
            break;
        }
        case SDL_CONTROLLERDEVICEREMOVED: {
            std::lock_guard<std::mutex> lock(g_controllerMutex);
            if (g_controller && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_controller)) == e.cdevice.which) {
                fprintf(stderr, "[manette] débranchée\n");
                SDL_GameControllerClose(g_controller);
                g_controller = nullptr;
                OpenFirstController();
            }
            break;
        }
        }
    }
    int16_t axes[6] = {};
    uint32_t buttons = 0;
    if (g_controller) {
        for (int i = 0; i < 6; i++) axes[i] = SDL_GameControllerGetAxis(g_controller, SDL_GameControllerAxis(i));
        for (int i = 0; i < SDL_CONTROLLER_BUTTON_DPAD_RIGHT + 1; i++)
            if (SDL_GameControllerGetButton(g_controller, SDL_GameControllerButton(i))) buttons |= 1u << i;
    }
    // Clavier : seulement fenêtre active (SDL remet les touches à zéro à la perte du focus)
    const uint8_t* keys = g_window && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS) ? SDL_GetKeyboardState(nullptr) : nullptr;
    // « Quitter » à la manette : BACK + START maintenus 3 secondes
    static std::chrono::steady_clock::time_point quitSince{};
    const uint32_t combo = (1u << SDL_CONTROLLER_BUTTON_BACK) | (1u << SDL_CONTROLLER_BUTTON_START);
    if ((buttons & combo) != combo) quitSince = {};
    else if (quitSince == std::chrono::steady_clock::time_point{}) quitSince = now;
    else if (now - quitSince >= std::chrono::seconds(3)) Quit(kExitOk, "Quitter à la manette (BACK + START maintenus)");
    const Pad p = MapPad(axes, buttons, keys);
    std::lock_guard<std::mutex> lock(g_padMutex);
    g_pad = p;
}

// Option vibrations : moteurs gauche (basse fréquence) et droit (haute fréquence), 0 à 65535.
void Rumble(uint16_t left, uint16_t right) {
    std::lock_guard<std::mutex> lock(g_controllerMutex);
    if (g_controller && config::g.vibration) SDL_GameControllerRumble(g_controller, left, right, 1000);
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
void Rumble(uint16_t, uint16_t) {}
#endif

}  // namespace wk::window
