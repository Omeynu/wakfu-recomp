// Test de la correspondance manette SDL / clavier -> état XINPUT (kernel/pad_map.cpp).
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>
#include <cstdio>
#include "../src/config.h"
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
    const uint32_t b = 1u << SDL_GAMEPAD_BUTTON_SOUTH | 1u << SDL_GAMEPAD_BUTTON_START |
                       1u << SDL_GAMEPAD_BUTTON_LEFT_SHOULDER | 1u << SDL_GAMEPAD_BUTTON_DPAD_UP |
                       1u << SDL_GAMEPAD_BUTTON_RIGHT_STICK;
    p = MapPad(axes, b, nullptr);
    Expect("boutons", p.buttons, 0x1000 | 0x10 | 0x100 | 0x1 | 0x80);
    // Axes : Y inversé (SDL bas positif), -32768 borné, gâchettes 0..32767 -> 0..255
    axes[SDL_GAMEPAD_AXIS_LEFTX] = 1000;
    axes[SDL_GAMEPAD_AXIS_LEFTY] = -32768;
    axes[SDL_GAMEPAD_AXIS_RIGHTY] = 32767;
    axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = 32767;
    axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = 16384;
    p = MapPad(axes, 0, nullptr);
    Expect("lx", p.lx, 1000);
    Expect("ly (haut)", p.ly, 32767);
    Expect("ry (bas)", p.ry, -32767);
    Expect("lt plein", p.lt, 255);
    Expect("rt moitié", p.rt, 128);
    // Clavier : Entrée = A, Échap = B, Espace = START, (D) = stick gauche à droite, (W)+(S) = neutre
    static bool keys[SDL_SCANCODE_COUNT];
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
    bool k2[SDL_SCANCODE_COUNT] = {};
    k2[SDL_SCANCODE_RETURN] = 1;
    Expect("Entrée n'est plus A", MapPad(zero, 0, k2).buttons, 0);
    k2[SDL_SCANCODE_RETURN] = 0; k2[SDL_SCANCODE_F] = 1; k2[SDL_SCANCODE_U] = 1;
    p = MapPad(zero, 0, k2);
    Expect("F = A", p.buttons, 0x1000);
    Expect("U = stick gauche haut", p.ly, 32767);
    k2[SDL_SCANCODE_F] = 0; k2[SDL_SCANCODE_U] = 0; k2[SDL_SCANCODE_ESCAPE] = 1;
    Expect("B inchangé (Échap)", MapPad(zero, 0, k2).buttons, 0x2000);
    // Zone morte (option zone_morte) : 0 = inchangé ; 0,2 : petit mouvement -> repos, plein -> plein, 0,6 -> 0,5
    int16_t s[6] = {};
    s[SDL_GAMEPAD_AXIS_LEFTX] = 5000;
    s[SDL_GAMEPAD_AXIS_LEFTY] = -3000;
    s[SDL_GAMEPAD_AXIS_RIGHTX] = -32768;
    p = MapPad(s, 0, nullptr, 0.0f);
    Expect("zone 0 : lx inchangé", p.lx, 5000);
    Expect("zone 0 : ly inchangé", p.ly, 3000);
    Expect("zone 0 : rx inchangé", p.rx, -32768);
    p = MapPad(s, 0, nullptr, 0.2f);
    Expect("zone 0,2 : lx repos", p.lx, 0);
    Expect("zone 0,2 : ly repos", p.ly, 0);
    Expect("zone 0,2 : rx plein", p.rx, -32768);
    s[SDL_GAMEPAD_AXIS_LEFTX] = 19660;   // 0,6
    s[SDL_GAMEPAD_AXIS_LEFTY] = 0;
    p = MapPad(s, 0, nullptr, 0.2f);
    Expect("zone 0,2 : 0,6 -> 0,5", p.lx, 16383);
    s[SDL_GAMEPAD_AXIS_LEFTX] = s[SDL_GAMEPAD_AXIS_LEFTY] = 32767;   // diagonale : direction gardée, bornée
    p = MapPad(s, 0, nullptr, 0.5f);
    Expect("zone 0,5 : diagonale x", p.lx, 32767);
    Expect("zone 0,5 : diagonale y", p.ly, -32768);
    // Option [manette] : chaque nom accepté par config.cpp est connu de SDL
    for (const char* const* n = wk::config::kPadBoutons; *n; n++)
        Expect(*n, wk::window::SetPadBindings({{"a", *n}}), 1);
    Expect("[manette] nom inconnu refusé", wk::window::SetPadBindings({{"a", "pasunbouton"}, {"voler", "b"}}), 0);
    int16_t z[6] = {};
    // A <- B physique, B <- A physique (échange), lt <- bouton X, START <- gâchette droite, Y <- aucun, rb doublé sur A
    Expect("[manette] appliqués", wk::window::SetPadBindings({{"a", "b"}, {"b", "a"}, {"lt", "x"}, {"start", "righttrigger"},
                                                              {"y", "aucun"}, {"rb", "b"}}), 6);
    p = MapPad(z, 1u << SDL_GAMEPAD_BUTTON_EAST, nullptr);
    Expect("B physique -> A et RB (doublon)", p.buttons, 0x1000 | 0x200);
    Expect("A physique -> B", MapPad(z, 1u << SDL_GAMEPAD_BUTTON_SOUTH, nullptr).buttons, 0x2000);
    Expect("Y physique -> rien", MapPad(z, 1u << SDL_GAMEPAD_BUTTON_NORTH, nullptr).buttons, 0);
    Expect("X physique -> lt plein", MapPad(z, 1u << SDL_GAMEPAD_BUTTON_WEST, nullptr).lt, 255);
    Expect("X physique -> X gardé", MapPad(z, 1u << SDL_GAMEPAD_BUTTON_WEST, nullptr).buttons, 0x4000);
    z[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = 3000;   // sous le seuil XINPUT (30/255)
    Expect("gâchette droite légère -> pas START", MapPad(z, 0, nullptr).buttons, 0);
    z[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = 20000;
    p = MapPad(z, 0, nullptr);
    Expect("gâchette droite -> START", p.buttons, 0x10);
    Expect("gâchette droite -> rt gardé", p.rt, 156);
    z[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = 16384;
    Expect("gâchette gauche -> rien (lt pris par X)", MapPad(z, 0, nullptr).lt, 0);
    wk::window::SetPadBindings({});
    Expect("[manette] défauts rétablis", MapPad(z, 1u << SDL_GAMEPAD_BUTTON_SOUTH, nullptr).buttons, 0x1000);
    if (g_fail) return 1;
    printf("unit_pad : OK\n");
    return 0;
}
