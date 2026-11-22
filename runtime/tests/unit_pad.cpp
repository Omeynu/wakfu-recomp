// Test de la correspondance manette SDL / clavier -> état XINPUT (kernel/pad_map.cpp).
#include <SDL_gamecontroller.h>
#include <SDL_scancode.h>
#include <cstdio>
#include "../src/kernel/window.h"
using wk::window::MapPad;
static int g_fail = 0;
static void Expect(const char* what, int got, int want) {
    if (got != want) { printf("ÉCHEC %s : %d (0x%X) au lieu de %d (0x%X)\n", what, got, got, want, want); g_fail++; }
}
int main() {
    int16_t axes[6] = {};
    // Repos
    wk::window::Pad p = MapPad(axes, 0, nullptr);
    Expect("repos boutons", p.buttons, 0);
    Expect("repos lt", p.lt, 0);
    // Boutons : A, START, LB, croix haut, clic stick droit
    const uint32_t b = 1u << SDL_CONTROLLER_BUTTON_A | 1u << SDL_CONTROLLER_BUTTON_START |
                       1u << SDL_CONTROLLER_BUTTON_LEFTSHOULDER | 1u << SDL_CONTROLLER_BUTTON_DPAD_UP |
                       1u << SDL_CONTROLLER_BUTTON_RIGHTSTICK;
    p = MapPad(axes, b, nullptr);
    Expect("boutons", p.buttons, 0x1000 | 0x10 | 0x100 | 0x1 | 0x80);
    // Axes : Y inversé (SDL bas positif), -32768 borné, gâchettes 0..32767 -> 0..255
    axes[SDL_CONTROLLER_AXIS_LEFTX] = 1000;
    axes[SDL_CONTROLLER_AXIS_LEFTY] = -32768;
    axes[SDL_CONTROLLER_AXIS_RIGHTY] = 32767;
    axes[SDL_CONTROLLER_AXIS_TRIGGERLEFT] = 32767;
    axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] = 16384;
    p = MapPad(axes, 0, nullptr);
    Expect("lx", p.lx, 1000);
    Expect("ly (haut)", p.ly, 32767);
    Expect("ry (bas)", p.ry, -32767);
    Expect("lt plein", p.lt, 255);
    Expect("rt moitié", p.rt, 128);
    // Clavier : Entrée = A, Échap = B, Espace = START, (D) = stick gauche à droite, (W)+(S) = neutre
    static uint8_t keys[SDL_NUM_SCANCODES];
    keys[SDL_SCANCODE_RETURN] = keys[SDL_SCANCODE_ESCAPE] = keys[SDL_SCANCODE_SPACE] = keys[SDL_SCANCODE_DOWN] = 1;
    keys[SDL_SCANCODE_D] = keys[SDL_SCANCODE_W] = keys[SDL_SCANCODE_S] = keys[SDL_SCANCODE_END] = 1;
    p = MapPad(axes, 0, keys);
    Expect("clavier boutons", p.buttons, 0x1000 | 0x2000 | 0x10 | 0x2);
    Expect("clavier lx", p.lx, 32767);
    Expect("clavier ly neutre (axe manette gardé)", p.ly, 32767);
    Expect("clavier rt", p.rt, 255);
    // Option [touches] : A sur F (Entrée ne vaut plus A), stick gauche haut sur U, touche inconnue ignorée
    const int applied = wk::window::SetKeyBindings({{"a", "F"}, {"stick_gauche_haut", "U"}, {"b", "PasUneTouche"}});
    Expect("touches appliquées", applied, 2);
    int16_t zero[6] = {};
    uint8_t k2[SDL_NUM_SCANCODES] = {};
    k2[SDL_SCANCODE_RETURN] = 1;
    Expect("Entrée n'est plus A", MapPad(zero, 0, k2).buttons, 0);
    k2[SDL_SCANCODE_RETURN] = 0; k2[SDL_SCANCODE_F] = 1; k2[SDL_SCANCODE_U] = 1;
    p = MapPad(zero, 0, k2);
    Expect("F = A", p.buttons, 0x1000);
    Expect("U = stick gauche haut", p.ly, 32767);
    k2[SDL_SCANCODE_F] = 0; k2[SDL_SCANCODE_U] = 0; k2[SDL_SCANCODE_ESCAPE] = 1;
    Expect("B inchangé (Échap)", MapPad(zero, 0, k2).buttons, 0x2000);
    if (g_fail) return 1;
    printf("unit_pad : OK\n");
    return 0;
}
