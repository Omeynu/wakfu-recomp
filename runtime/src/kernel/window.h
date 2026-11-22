// Fenêtre SDL2 (présentation du tampon d'image, fermeture) et entrées de l'hôte (manette SDL, clavier).
// Fenêtre, événements et présentation : thread du processeur de commandes. PadState : thread quelconque.
// Sans SDL2, sans écran ou fenêtre non demandée : rien n'est ouvert, PadState reste au repos.
#pragma once
#include <cstdint>
#include <map>
#include <string>

namespace wk::window {

// WAKFU_WINDOW=1 : oui ; =0 : non ; absent : oui sauf capture/test (WAKFU_CAPTURE, WAKFU_STOP_AT_*, WAKFU_INPUT)
bool Wanted();
// Vidéo + manettes SDL, fenêtre 1280x720 redimensionnable ; fixe les extensions d'instance Vulkan
// de la surface (avant l'initialisation du rendu).
void Open();
bool IsOpen();                              // fenêtre créée
void Pump();                                // événements (au plus toutes les 4 ms) ; fermeture -> sortie
void Present(const uint32_t fetch[6]);      // constante de texture du tampon d'image -> fenêtre

// État façon X_INPUT_GAMEPAD (boutons XINPUT, gâchettes 0-255, sticks haut = positif)
struct Pad {
    uint16_t buttons = 0;
    uint8_t lt = 0, rt = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    bool operator==(const Pad&) const = default;
};
Pad PadState();
void Rumble(uint16_t left, uint16_t right);   // vibrations (option [entrees] vibrations)
// Correspondance SDL -> XINPUT (pad_map.cpp) : axes SDL (gauche X/Y, droite X/Y, gâchettes G/D),
// boutons SDL (bit i = SDL_CONTROLLER_BUTTON i), état du clavier par scancode SDL (nul : pas de clavier).
Pad MapPad(const int16_t axes[6], uint32_t buttons, const uint8_t* keys);
// Option [touches] : commande -> nom de touche SDL ; remplace les touches par défaut de la commande. Retourne le nombre
// de commandes appliquées (touche inconnue : message, ignorée).
int SetKeyBindings(const std::map<std::string, std::string>& bindings);

}  // namespace wk::window
