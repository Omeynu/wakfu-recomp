// Manette SDL et clavier -> état XINPUT (voir window.h). Clavier par position physique (scancodes),
// indépendant de la disposition : noms QWERTY entre parenthèses, AZERTY ensuite.
#include "window.h"
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

namespace wk::window {

namespace {
// Boutons XINPUT
constexpr uint16_t kUp = 0x1, kDown = 0x2, kLeft = 0x4, kRight = 0x8, kStart = 0x10, kBack = 0x20,
                   kLThumb = 0x40, kRThumb = 0x80, kLB = 0x100, kRB = 0x200, kA = 0x1000, kB = 0x2000,
                   kX = 0x4000, kY = 0x8000;

// Clavier de secours : flèches = croix, Entrée = A, Échap = B, (X) = X, (Y) = Y, Espace = START,
// Retour arrière / Tab = BACK, Page préc./suiv. = LB/RB, Début/Fin = LT/RT,
// (W A S D) = (Z Q S D) = stick gauche, (I J K L) = stick droit, (C) / (V) = clic des sticks.
// Remplaçable commande par commande (option [touches], SetKeyBindings).
struct KeyButton { SDL_Scancode k; uint16_t x; };
std::vector<KeyButton> g_keyButtons = {
    {SDL_SCANCODE_UP, kUp}, {SDL_SCANCODE_DOWN, kDown}, {SDL_SCANCODE_LEFT, kLeft}, {SDL_SCANCODE_RIGHT, kRight},
    {SDL_SCANCODE_RETURN, kA}, {SDL_SCANCODE_KP_ENTER, kA}, {SDL_SCANCODE_ESCAPE, kB},
    {SDL_SCANCODE_X, kX}, {SDL_SCANCODE_Y, kY}, {SDL_SCANCODE_SPACE, kStart},
    {SDL_SCANCODE_BACKSPACE, kBack}, {SDL_SCANCODE_TAB, kBack},
    {SDL_SCANCODE_PAGEUP, kLB}, {SDL_SCANCODE_PAGEDOWN, kRB}, {SDL_SCANCODE_C, kLThumb}, {SDL_SCANCODE_V, kRThumb}};
SDL_Scancode g_lt = SDL_SCANCODE_HOME, g_rt = SDL_SCANCODE_END;
// sticks : gauche puis droit ; haut, bas, gauche, droite
SDL_Scancode g_stick[2][4] = {{SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_A, SDL_SCANCODE_D},
                              {SDL_SCANCODE_I, SDL_SCANCODE_K, SDL_SCANCODE_J, SDL_SCANCODE_L}};

// Option [manette] : source de chaque bouton du jeu, dans l'ordre de config::kPadControls (a b x y lb rb lt rt back
// start ls rs haut bas gauche droite) : bouton SDL, kGachette + axe SDL (gâchette) ou -1 (aucun).
constexpr int kGachette = 64;
constexpr std::array<int, 16> kDefaut = {
    SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    kGachette + SDL_GAMEPAD_AXIS_LEFT_TRIGGER, kGachette + SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
    SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK,
    SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT};
constexpr const char* kPadNoms[16] = {"a", "b", "x", "y", "lb", "rb", "lt", "rt", "back", "start", "ls", "rs",
                                      "haut", "bas", "gauche", "droite"};
std::array<int, 16> g_source = kDefaut;
constexpr int kSeuilGachette = 30 * 32767 / 255;   // gâchette physique vue comme un bouton : seuil XINPUT (30 sur 255)

int16_t FlipY(int16_t v) { return v == -32768 ? 32767 : int16_t(-v); }   // SDL : bas positif ; XINPUT : haut positif
uint8_t Trigger(int16_t v) { return v <= 0 ? 0 : uint8_t((int(v) * 255 + 16383) / 32767); }
// Zone morte radiale : amplitude <= zm -> repos ; au-delà, (amplitude - zm) / (1 - zm) dans la même direction.
void ZoneMorte(int16_t* x, int16_t* y, float zm) {
    if (zm <= 0.0f) return;
    const float m = std::hypot(float(*x), float(*y)) / 32767.0f;
    const float k = m <= zm ? 0.0f : (m - zm) / (1.0f - zm) / m;
    *x = int16_t(std::clamp(std::lround(*x * k), -32768L, 32767L));
    *y = int16_t(std::clamp(std::lround(*y * k), -32768L, 32767L));
}
}  // namespace

Pad MapPad(const int16_t axesPhys[6], uint32_t boutonsPhys, const bool* keys, float zoneMorte) {
    // Option [manette] : boutons et gâchettes physiques -> boutons du jeu (numérotés comme SDL), avant XINPUT
    int16_t axes[6];
    std::copy(axesPhys, axesPhys + 6, axes);
    uint32_t buttons = 0;
    for (int i = 0; i < 16; i++) {
        const int s = g_source[i], d = kDefaut[i];
        const int v = s < 0 ? 0 : s >= kGachette ? axesPhys[s - kGachette] : (boutonsPhys >> s & 1) * 32767;
        if (d >= kGachette) axes[d - kGachette] = int16_t(v);
        else if (v > kSeuilGachette) buttons |= 1u << d;
    }
    static const struct { int sdl; uint16_t x; } kButtons[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, kA}, {SDL_GAMEPAD_BUTTON_EAST, kB}, {SDL_GAMEPAD_BUTTON_WEST, kX},
        {SDL_GAMEPAD_BUTTON_NORTH, kY}, {SDL_GAMEPAD_BUTTON_BACK, kBack}, {SDL_GAMEPAD_BUTTON_START, kStart},
        {SDL_GAMEPAD_BUTTON_LEFT_STICK, kLThumb}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, kRThumb},
        {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kLB}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kRB},
        {SDL_GAMEPAD_BUTTON_DPAD_UP, kUp}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, kDown},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, kLeft}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kRight}};
    Pad p;
    for (auto& b : kButtons)
        if (buttons >> b.sdl & 1) p.buttons |= b.x;
    p.lx = axes[SDL_GAMEPAD_AXIS_LEFTX];
    p.ly = FlipY(axes[SDL_GAMEPAD_AXIS_LEFTY]);
    p.rx = axes[SDL_GAMEPAD_AXIS_RIGHTX];
    p.ry = FlipY(axes[SDL_GAMEPAD_AXIS_RIGHTY]);
    ZoneMorte(&p.lx, &p.ly, zoneMorte);
    ZoneMorte(&p.rx, &p.ry, zoneMorte);
    p.lt = Trigger(axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER]);
    p.rt = Trigger(axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER]);
    if (!keys) return p;

    for (auto& k : g_keyButtons)
        if (keys[k.k]) p.buttons |= k.x;
    if (keys[g_lt]) p.lt = 255;
    if (keys[g_rt]) p.rt = 255;
    auto stick = [keys](SDL_Scancode neg, SDL_Scancode pos, int16_t* v) {
        if (keys[neg] != keys[pos]) *v = keys[pos] ? 32767 : -32768;
    };
    stick(g_stick[0][2], g_stick[0][3], &p.lx);
    stick(g_stick[0][1], g_stick[0][0], &p.ly);
    stick(g_stick[1][2], g_stick[1][3], &p.rx);
    stick(g_stick[1][1], g_stick[1][0], &p.ry);
    return p;
}

int SetKeyBindings(const std::map<std::string, std::string>& bindings) {
    static const struct { const char* name; uint16_t x; } kButtonNames[] = {
        {"a", kA}, {"b", kB}, {"x", kX}, {"y", kY}, {"start", kStart}, {"back", kBack}, {"lb", kLB}, {"rb", kRB},
        {"haut", kUp}, {"bas", kDown}, {"gauche", kLeft}, {"droite", kRight}, {"clic_gauche", kLThumb}, {"clic_droit", kRThumb}};
    static const char* const kDirs[4] = {"haut", "bas", "gauche", "droite"};
    int applied = 0;
    for (auto& [name, keyName] : bindings) {
        const SDL_Scancode sc = SDL_GetScancodeFromName(keyName.c_str());
        if (sc == SDL_SCANCODE_UNKNOWN) { fprintf(stderr, "[config] [touches] %s : touche « %s » inconnue\n", name.c_str(), keyName.c_str()); continue; }
        bool done = false;
        for (auto& b : kButtonNames)
            if (name == b.name) {
                std::erase_if(g_keyButtons, [&](const KeyButton& k) { return k.x == b.x; });
                g_keyButtons.push_back({sc, b.x});
                done = true;
            }
        if (name == "lt") { g_lt = sc; done = true; }
        if (name == "rt") { g_rt = sc; done = true; }
        for (int s = 0; s < 2; s++)
            for (int d = 0; d < 4; d++)
                if (name == std::string(s ? "stick_droit_" : "stick_gauche_") + kDirs[d]) { g_stick[s][d] = sc; done = true; }
        applied += done;
    }
    return applied;
}

int SetPadBindings(const std::map<std::string, std::string>& bindings) {
    g_source = kDefaut;
    int applied = 0;
    for (auto& [name, bouton] : bindings) {
        int i = 0;
        while (i < 16 && name != kPadNoms[i]) i++;
        const SDL_GamepadButton b = SDL_GetGamepadButtonFromString(bouton.c_str());
        const SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(bouton.c_str());
        const bool gachette = a == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || a == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
        if (i == 16 || (bouton != "aucun" && b == SDL_GAMEPAD_BUTTON_INVALID && !gachette)) {
            fprintf(stderr, "[config] [manette] %s : bouton « %s » inconnu\n", name.c_str(), bouton.c_str());
            continue;
        }
        g_source[i] = bouton == "aucun" ? -1 : gachette ? kGachette + a : int(b);
        applied++;
    }
    return applied;
}

}  // namespace wk::window
