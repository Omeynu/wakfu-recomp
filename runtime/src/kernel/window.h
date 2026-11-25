// Fenêtre SDL3 (présentation de l'image native, fermeture) et entrées de l'hôte (manette SDL, clavier).
// Fenêtre et événements : thread du processeur de commandes ; présentation : fil natif. PadState : thread quelconque.
// Sans SDL3, sans écran ou fenêtre non demandée : rien n'est ouvert, PadState reste au repos.
#pragma once
#include <cstdint>
#include <map>
#include <string>

typedef struct VkImage_T* VkImage;   // poignée Vulkan, sans inclure vulkan.h
namespace wk::window {

// WAKFU_WINDOW=1 : oui ; =0 : non ; absent : oui sauf capture/test (WAKFU_CAPTURE, WAKFU_STOP_AT_*, WAKFU_INPUT)
bool Wanted();
// Vidéo + manettes SDL, fenêtre 1280x720 redimensionnable ; fixe les extensions d'instance Vulkan
// de la surface (avant l'initialisation du rendu).
void Open();
bool IsOpen();                              // fenêtre créée
bool Focused();                             // fenêtre au premier plan (vrai sans fenêtre ; thread quelconque)
bool CaptureDemandee();                     // touche de capture appuyée depuis le dernier appel (option touche_capture)
void CaptureFaite();                        // capture écrite : message « Capture effectuée » affiché 2 s
void Pump();                                // événements (au plus toutes les 4 ms) ; fermeture -> sortie
void PresentNative(VkImage image, uint32_t w, uint32_t h);   // image native (fil natif) -> fenêtre

// État façon X_INPUT_GAMEPAD (boutons XINPUT, gâchettes 0-255, sticks haut = positif)
struct Pad {
    uint16_t buttons = 0;
    uint8_t lt = 0, rt = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    bool operator==(const Pad&) const = default;
};
// Joueur 0 ou 1 (options [entrees] joueur1, joueur2). Le joueur 1 est toujours branché ; le joueur 2 l'est si sa
// manette est branchée ou s'il a le clavier.
Pad PadState(int joueur = 0);
bool PadPresent(int joueur);
void Rumble(int joueur, uint16_t left, uint16_t right);   // vibrations (option [entrees] vibrations)
// Correspondance SDL -> XINPUT (pad_map.cpp) : axes SDL (gauche X/Y, droite X/Y, gâchettes G/D),
// boutons SDL (bit i = SDL_GamepadButton i, A B X Y par étiquette), état du clavier par scancode SDL (nul : pas de clavier).
// zoneMorte (option [entrees] zone_morte, 0 à 0,5) : zone morte radiale des sticks de la manette, course restante
// remise à l'échelle (0 : axes inchangés).
Pad MapPad(const int16_t axes[6], uint32_t buttons, const bool* keys, float zoneMorte = 0.0f);
// Option [touches] : commande -> nom de touche SDL ; remplace les touches par défaut de la commande. Retourne le nombre
// de commandes appliquées (touche inconnue : message, ignorée).
int SetKeyBindings(const std::map<std::string, std::string>& bindings);
// Option [manette] : bouton du jeu -> nom SDL du bouton physique (gâchettes : lefttrigger, righttrigger ; « aucun »).
// Repart des boutons par défaut. Retourne le nombre de boutons appliqués (nom inconnu : message, ignoré).
int SetPadBindings(const std::map<std::string, std::string>& bindings);

}  // namespace wk::window
