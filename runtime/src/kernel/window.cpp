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
#ifdef WAKFU_HAS_SDL3
#include <SDL3/SDL.h>
#ifdef WAKFU_HAS_SHADERGEN
#include <SDL3/SDL_vulkan.h>
#include "../gpu/present.h"
#include "../natif/device.h"
#include "../natif/icones.h"
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
#if defined(WAKFU_HAS_SDL3) && defined(WAKFU_HAS_SHADERGEN)
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

#ifdef WAKFU_HAS_SDL3
namespace {
SDL_Window* g_window = nullptr;
// Manettes 1 à 4 : place libre au branchement, gardée jusqu'au débranchement (attribution stable)
SDL_Gamepad* g_controllers[4] = {};
std::mutex g_controllerMutex;   // g_controllers : changé par Pump, utilisé par Rumble (thread du jeu)
bool g_sdl = false;
SDL_Scancode g_toucheCapture = SDL_SCANCODE_UNKNOWN;   // option touche_capture

// Écran n (option ecran : rang dans la liste des écrans, comme l'index SDL2) -> identifiant SDL3 ; 0 : absent
SDL_DisplayID EcranId(int n) {
    int nb = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&nb);
    const SDL_DisplayID id = ids && n >= 0 && n < nb ? ids[n] : 0;
    SDL_free(ids);
    return id;
}
int NbEcrans() {
    int nb = 0;
    SDL_free(SDL_GetDisplays(&nb));
    return nb;
}

// Options ecran, position_x, position_y, largeur, hauteur : mémorisées à la fermeture (mode fenêtré, fenêtre ni
// agrandie ni réduite), position relative à l'écran.
void MemoriserFenetre() {
    if (!g_window || config::g.mode != "fenetre" ||
        (SDL_GetWindowFlags(g_window) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED | SDL_WINDOW_FULLSCREEN)))
        return;
    const SDL_DisplayID id = SDL_GetDisplayForWindow(g_window);
    int d = 0;
    while (EcranId(d) && EcranId(d) != id) d++;
    SDL_Rect b;
    if (!id || !EcranId(d) || !SDL_GetDisplayBounds(id, &b)) return;
    int x = 0, y = 0, w = 0, h = 0;
    SDL_GetWindowPosition(g_window, &x, &y);
    SDL_GetWindowSize(g_window, &w, &h);
    // Wayland : position de la fenêtre inconnue (toujours 0,0) : celle du fichier est gardée
    if (strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0) config::SaveWindow(g_dataDir, d, std::nullopt, std::nullopt, w, h);
    else config::SaveWindow(g_dataDir, d, x - b.x, y - b.y, w, h);
}

// Manette du joueur (option joueur1 / joueur2 : "1" à "4"), nulle pour "clavier" ou manette absente
SDL_Gamepad* ManetteDu(int joueur) {
    const std::string& j = config::g.joueurs[joueur];
    return j == "clavier" ? nullptr : g_controllers[j[0] - '1'];
}
// Joueur qui reçoit le clavier : celui qui a "clavier", sinon le joueur 1
int JoueurClavier() { return config::g.joueurs[1] == "clavier" ? 1 : 0; }
#ifdef WAKFU_HAS_SHADERGEN
// Option [entrees] icones : PlayStation si demandé ; auto : manette PlayStation au joueur 1, sinon RECOMPLAUNCHER_PAD
bool IconesPlayStation() {
    if (config::g.icones != "auto") return config::g.icones == "playstation";
    if (SDL_Gamepad* c = ManetteDu(0)) {
        const SDL_GamepadType t = SDL_GetGamepadType(c);
        return t == SDL_GAMEPAD_TYPE_PS3 || t == SDL_GAMEPAD_TYPE_PS4 || t == SDL_GAMEPAD_TYPE_PS5;
    }
    const char* e = getenv("RECOMPLAUNCHER_PAD");
    return e && std::string(e) == "playstation";
}
#endif
void JournalAttribution() {
#ifdef WAKFU_HAS_SHADERGEN
    static int avant = -1;
    const bool ps = IconesPlayStation();
    natif::ChoisirIcones(ps);
    if (avant != ps) fprintf(stderr, "[icônes] %s (option %s)\n", ps ? "PlayStation" : "Xbox (celles du jeu)", config::g.icones.c_str());
    avant = ps;
#endif
    for (int j = 0; j < 2; j++) {
        const std::string& src = config::g.joueurs[j];
        SDL_Gamepad* c = ManetteDu(j);
        fprintf(stderr, "[entrées] joueur %d : %s%s%s%s%s\n", j + 1, src == "clavier" ? "clavier" : ("manette " + src).c_str(),
                src == "clavier" ? "" : c ? " (" : " (absente)", c ? SDL_GetGamepadName(c) : "", c ? ")" : "",
                src != "clavier" && JoueurClavier() == j ? " + clavier" : "");
    }
}
void OpenController(SDL_JoystickID id) {
    if (!SDL_IsGamepad(id)) return;
    for (SDL_Gamepad* c : g_controllers)
        if (c && SDL_GetGamepadID(c) == id) return;   // déjà ouverte
    for (int n = 0; n < 4; n++)
        if (!g_controllers[n]) {
            if (!(g_controllers[n] = SDL_OpenGamepad(id))) return;
            fprintf(stderr, "[manette] %d : %s\n", n + 1, SDL_GetGamepadName(g_controllers[n]));
            JournalAttribution();
            return;
        }
}
}  // namespace

void Open() {
    SetKeyBindings(config::g.keys);   // option [touches]
    SetPadBindings(config::g.pad);    // option [manette]
    g_toucheCapture = SDL_GetScancodeFromName(config::g.captureKey.c_str());
    if (g_toucheCapture == SDL_SCANCODE_UNKNOWN)
        fprintf(stderr, "[config] [confort] touche_capture : touche « %s » inconnue (pas de capture)\n", config::g.captureKey.c_str());
    // Option entree_arriere_plan : la manette reste lue quand la fenêtre n'a pas le focus
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, config::g.backgroundInput ? "1" : "0");
    if (SDL_InitSubSystem(SDL_INIT_GAMEPAD)) g_sdl = true, JournalAttribution();
    else fprintf(stderr, "[manette] SDL : %s (pas de manette)\n", SDL_GetError());
#ifdef WAKFU_HAS_SHADERGEN
    // Windows : fenêtre non déclarée « DPI » comme sous SDL2 (taille en points, mise à l'échelle par le système)
    SDL_SetHint("SDL_WINDOWS_DPI_AWARENESS", "unaware");
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        fprintf(stderr, "[fenetre] SDL : %s (pas de fenêtre)\n", SDL_GetError());
        return;
    }
    g_sdl = true;
    // Options [video] largeur, hauteur, mode (plein_ecran : changement de mode d'affichage ; sans_bordure : bureau)
    const bool pleinEcran = config::g.mode == "plein_ecran" || config::g.mode == "sans_bordure";
    // Options ecran, position_x, position_y : écran absent -> principal ; position hors de l'écran -> centrée
    int ecran = config::g.screen;
    if (ecran >= NbEcrans()) {
        fprintf(stderr, "[config] [video] ecran = %d : écran absent (%d écran(s)), écran 0\n", ecran, NbEcrans());
        ecran = 0;
    }
    const SDL_DisplayID ecranId = EcranId(ecran);
    int x = int(SDL_WINDOWPOS_CENTERED_DISPLAY(ecranId)), y = x;
    SDL_Rect b;
    if (config::g.posX && config::g.posY && config::g.mode == "fenetre" && SDL_GetDisplayBounds(ecranId, &b) &&
        *config::g.posX >= 0 && *config::g.posY >= 0 && *config::g.posX < b.w - 64 && *config::g.posY < b.h - 64)
        x = b.x + *config::g.posX, y = b.y + *config::g.posY;
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "Islands of Wakfu");
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, x);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, y);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, config::g.width);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, config::g.height);
    // Cachée jusqu'au choix du mode plein écran : affichée une seule fois, dans son état final
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE |
                          SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN | (pleinEcran ? SDL_WINDOW_FULLSCREEN : 0));
    g_window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (!g_window) { fprintf(stderr, "[fenetre] création : %s (pas de fenêtre)\n", SDL_GetError()); return; }
    // plein_ecran : mode d'affichage le plus proche de largeur x hauteur (SDL2 : SDL_WINDOW_FULLSCREEN) ; sinon bureau
    SDL_DisplayMode mode;
    if (config::g.mode == "plein_ecran" &&
        (!SDL_GetClosestFullscreenDisplayMode(ecranId, config::g.width, config::g.height, 0.0f, false, &mode) ||
         !SDL_SetWindowFullscreenMode(g_window, &mode)))
        fprintf(stderr, "[fenetre] mode %dx%d : %s (plein écran du bureau)\n", config::g.width, config::g.height, SDL_GetError());
    SDL_ShowWindow(g_window);
    fprintf(stderr, "[fenetre] %dx%d, mode %s, vsync %s, limite %d img/s, écran %d\n", config::g.width, config::g.height,
            config::g.mode.c_str(), config::g.vsync ? "oui" : "non", config::g.fpsLimit, ecran);
    Uint32 n = 0;
    const char* const* names = SDL_Vulkan_GetInstanceExtensions(&n);
    if (!names) {
        fprintf(stderr, "[fenetre] extensions Vulkan : %s\n", SDL_GetError());
        SDL_DestroyWindow(g_window);
        g_window = nullptr;
        return;
    }
    vk::SetInstanceExtensions(std::vector<std::string>(names, names + n));
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
        case SDL_EVENT_QUIT:
            MemoriserFenetre();
            Quit(kExitOk, "fenêtre fermée");
        case SDL_EVENT_WINDOW_FOCUS_LOST:
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            g_focus = e.type == SDL_EVENT_WINDOW_FOCUS_GAINED;   // option couper_hors_focus
            break;
#ifdef WAKFU_HAS_SHADERGEN
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: gpu::PresentResize(); break;
#endif
        case SDL_EVENT_KEY_DOWN:
            if (!e.key.repeat && e.key.scancode == g_toucheCapture && g_toucheCapture != SDL_SCANCODE_UNKNOWN) g_capture = true;
            break;
        case SDL_EVENT_GAMEPAD_ADDED: {   // which : identifiant d'instance (aussi pour les manettes déjà branchées)
            std::lock_guard<std::mutex> lock(g_controllerMutex);
            OpenController(e.gdevice.which);
            break;
        }
        case SDL_EVENT_GAMEPAD_REMOVED: {
            std::lock_guard<std::mutex> lock(g_controllerMutex);
            for (int n = 0; n < 4; n++)
                if (g_controllers[n] && SDL_GetGamepadID(g_controllers[n]) == e.gdevice.which) {
                    fprintf(stderr, "[manette] %d débranchée\n", n + 1);
                    SDL_CloseGamepad(g_controllers[n]);
                    g_controllers[n] = nullptr;
                    JournalAttribution();
                }
            break;
        }
        }
    }
    // Clavier : seulement fenêtre active (SDL remet les touches à zéro à la perte du focus)
    const bool* keys = g_window && (SDL_GetWindowFlags(g_window) & SDL_WINDOW_INPUT_FOCUS) ? SDL_GetKeyboardState(nullptr) : nullptr;
    // « Quitter » à la manette : BACK + START maintenus 3 secondes (sur une même manette)
    static std::chrono::steady_clock::time_point quitSince{};
    bool comboTenu = false;
    Pad pads[2];
    bool present2 = false;
    for (SDL_Gamepad* c : g_controllers)
        comboTenu |= c && SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_BACK) && SDL_GetGamepadButton(c, SDL_GAMEPAD_BUTTON_START);
    for (int j = 0; j < 2; j++) {
        int16_t axes[6] = {};
        uint32_t buttons = 0;
        SDL_Gamepad* c = ManetteDu(j);
        if (c) {
            for (int i = 0; i < 6; i++) axes[i] = SDL_GetGamepadAxis(c, SDL_GamepadAxis(i));
            for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; i++)   // tous : [manette] peut utiliser palettes, pavé tactile...
                if (SDL_GetGamepadButton(c, SDL_GamepadButton(i))) {
                    // Boutons de face par étiquette comme SDL2 (manettes Nintendo : A à droite = A) ; SDL3 : par position
                    const int l = SDL_GetGamepadButtonLabel(c, SDL_GamepadButton(i));
                    buttons |= 1u << (l >= SDL_GAMEPAD_BUTTON_LABEL_A && l <= SDL_GAMEPAD_BUTTON_LABEL_Y ? l - SDL_GAMEPAD_BUTTON_LABEL_A : i);
                }
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
    SDL_Gamepad* c = ManetteDu(joueur & 1);
    if (c && k > 0.0f) SDL_RumbleGamepad(c, uint16_t(left * k), uint16_t(right * k), 1000);
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
        if (!SDL_Vulkan_CreateSurface(g_window, ctx->instance, nullptr, &surface)) {
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
    SDL_GetWindowSizeInPixels(g_window, &dw, &dh);
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
void Open() { fprintf(stderr, "[fenetre] runtime compilé sans SDL3 : pas de fenêtre ni de manette\n"); }
bool IsOpen() { return false; }
void Pump() {}
void Present(const uint32_t*) {}
void PresentNative(VkImage, uint32_t, uint32_t) {}
void Rumble(int, uint16_t, uint16_t) {}
#endif

}  // namespace wk::window
