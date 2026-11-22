// Options du joueur : <données>/config.toml (valeurs par défaut écrites au premier lancement, documentées dans le
// fichier et dans docs/options.md). Par défaut, le comportement est celui d'avant les options (rendu inchangé).
#pragma once
#include <map>
#include <string>
#include <vector>

namespace wk::config {

struct Config {
    // [video]
    int width = 1280, height = 720;          // taille de la fenêtre (mode fenêtré)
    std::string mode = "fenetre";            // fenetre | plein_ecran | sans_bordure
    bool vsync = false;                      // synchronisation verticale (sinon MAILBOX / IMMEDIATE)
    int fpsLimit = 0;                        // limite d'images par seconde (0 = aucune)
    bool showFps = false;                    // images par seconde dans le titre de la fenêtre
    std::string gpu;                         // partie du nom du GPU Vulkan voulu (vide = automatique)
    int renderScale = 1;                     // échelle de rendu interne (1 à 4) : sur-échantillonnage, ramené au resolve
    // [post] (image affichée seulement ; les défauts ne changent rien)
    std::string antiAliasing = "aucun";      // aucun | fxaa (passe finale)
    std::string upscale = "lineaire";        // lineaire | fsr1 : agrandissement de l'image du jeu vers la fenêtre
    float sharpness = 0.0f;                  // netteté AMD CAS (0 = désactivée, 1 = maximale)
    float brightness = 1.0f;                 // multiplicateur de luminosité (0,5 à 2)
    float gamma = 1.0f;                      // gamma de sortie (0,5 à 3 ; > 1 éclaircit les tons moyens)
    bool xboxColor = false;                  // image lue comme codée par la courbe PWL du Xenos, réencodée en sRGB
    int anisotropy = 1;                      // filtrage anisotrope des textures (1, 2, 4, 8, 16) : change le rendu
    // [audio]
    float volume = 1.0f;                     // volume général (0 à 1), appliqué au mixage hôte
    // [entrees]
    bool vibration = true;                   // vibrations de la manette (XamInputSetState)
    bool backgroundInput = false;            // manette lue même sans le focus de la fenêtre
    // [confort]
    bool skipLogos = false;                  // logos d'introduction sautés
    bool console = true;                     // Windows : console du journal (sinon journal dans <données>/journal.txt)
    // [touches] commande de la manette -> nom de touche SDL (« Return », « Z »…), remplace les touches par défaut
    std::map<std::string, std::string> keys;
};

extern Config g;
extern const char* const kControls[];   // noms des commandes de [touches], terminé par nullptr

// Lit le texte TOML ; les clés inconnues ou invalides gardent la valeur par défaut et ajoutent un message à `errors`.
Config Parse(const std::string& text, std::vector<std::string>* errors);
// Texte TOML documenté des valeurs de `c` (écrit au premier lancement).
std::string Serialize(const Config& c);
// <dossier>/config.toml : lu s'il existe, sinon créé avec les valeurs par défaut. Remplit `g`.
void Load(const std::string& dataDir);

}  // namespace wk::config
