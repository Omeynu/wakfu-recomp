// Options du joueur : <données>/config.toml (valeurs par défaut écrites au premier lancement, documentées dans le
// fichier et dans docs/options.md). Par défaut, le comportement est celui d'avant les options (rendu inchangé).
#pragma once
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace wk::config {

struct Config {
    // [video]
    int width = 1280, height = 720;          // taille de la fenêtre (mode fenêtré)
    std::string mode = "fenetre";            // fenetre | plein_ecran | sans_bordure
    int screen = 0;                          // écran (index SDL) de la fenêtre
    std::optional<int> posX, posY;           // position dans l'écran (mémorisée à la fermeture) ; absente : centrée
    bool vsync = false;                      // synchronisation verticale (sinon MAILBOX / IMMEDIATE)
    int fpsLimit = 0;                        // limite d'images par seconde (0 = aucune)
    bool showFps = false;                    // images par seconde dans le titre de la fenêtre
    std::string gpu;                         // partie du nom du GPU Vulkan voulu (vide = automatique)
    std::string msaa = "auto";               // multi-échantillonnage demandé par le jeu : auto | active | desactive | 2x | 4x | 8x
    int renderScale = 1;                     // définition interne : cibles de rendu ×1 à ×4 (1280x720 à 5120x2880)
    int anisotropy = 1;                      // filtrage anisotrope des textures : 1 (désactivé), 2, 4, 8, 16
    std::string ombres = "origine";          // définition de la cible de l'ombre (zone 1) : origine | x2 | x4, en plus de renderScale
    // [post] (image affichée seulement ; les défauts ne changent rien)
    std::string antiAliasing = "aucun";      // aucun | fxaa (passe finale)
    std::string upscale = "lineaire";        // lineaire | fsr1 : agrandissement de l'image du jeu vers la fenêtre
    float sharpness = 0.0f;                  // netteté AMD CAS (0 = désactivée, 1 = maximale)
    float brightness = 1.0f;                 // multiplicateur de luminosité (0,5 à 2)
    float gamma = 1.0f;                      // gamma de sortie (0,5 à 3 ; > 1 éclaircit les tons moyens)
    bool xboxColor = false;                  // image lue comme codée par la courbe PWL du Xenos, réencodée en sRGB
    // [audio]
    float volume = 1.0f;                     // volume général (0 à 1), appliqué au mixage hôte
    bool muteUnfocused = false;              // volume 0 quand la fenêtre n'a pas le focus (mixage continu)
    // [entrees]
    float vibration = 1.0f;                  // intensité des vibrations de la manette (0 à 1 ; XamInputSetState)
    float deadZone = 0.0f;                   // zone morte radiale des sticks (0 à 0,5 ; 0 = axes SDL inchangés)
    bool backgroundInput = false;            // manette lue même sans le focus de la fenêtre
    // commandes des joueurs 1 et 2 (coopératif) : "1" à "4" (manette n°, ordre de branchement) ou "clavier" ;
    // le clavier commande le joueur 1 si aucun joueur n'a "clavier"
    std::string joueurs[2] = {"1", "2"};
    // [confort]
    bool skipLogos = false;                  // logos d'introduction sautés
    bool console = false;                    // Windows : console du journal (sinon journal dans <données>/journal.log)
    std::string captureKey = "F12";         // touche de capture d'écran (nom SDL) : PNG dans <données>/captures/
    // [jeu]
    std::string langue = "auto";             // auto (langue du système) | fr | en | de | es | it | ja
    // [touches] commande de la manette -> nom de touche SDL (« Return », « Z »…), remplace les touches par défaut
    std::map<std::string, std::string> keys;
};

extern Config g;

// GPU Vulkan vu par le jeu ou le lanceur ; type : VkPhysicalDeviceType (1 intégré, 2 dédié, 3 virtuel, 4 processeur)
struct GpuVu { std::string nom; int type = 0; bool vulkan13 = false; };
// Choix du GPU (même règle pour le jeu et le lanceur) : option gpu (partie du nom, casse ignorée), sinon dédié,
// puis intégré, puis autre ; GPU sans Vulkan 1.3 exclus. -1 : aucun. *optionTrouvee : faux si l'option ne désigne rien.
int ChoisirGpu(const std::vector<GpuVu>& gpus, const std::string& option, bool* optionTrouvee = nullptr);
// Échantillons du multi-échantillonnage retenu (0 : désactivé ; 2, 4 ou 8, avant plafond du GPU) : « active » = 4,
// « 2x » / « 4x » / « 8x », « desactive » (choix du joueur), « auto » : 4 sur GPU dédié seulement.
int MsaaEchantillons(const std::string& choix, int typeGpu);
// Préréglages image du lanceur (« auto », « economie », « equilibre », « qualite ») : echelle_rendu,
// multi_echantillonnage, filtrage_anisotrope, ombres. « auto » = valeurs par défaut (MSAA selon le GPU).
bool AppliquerPrereglage(const std::string& nom, Config* c);   // faux : nom inconnu, *c inchangé
std::string Prereglage(const Config& c);   // préréglage dont *c a les valeurs image, sinon « personnalise »
// Langue du jeu : code XC_LANGUAGE (1 anglais, 2 japonais, 3 allemand, 4 français, 5 espagnol, 6 italien).
// « auto » : `systeme` (« fr_FR », « de-DE »…) si le jeu a cette langue, sinon anglais.
int LangueXbox(const std::string& choix, const std::string& systeme);
std::string LangueSysteme();   // Windows : GetUserDefaultLocaleName ; ailleurs : LC_ALL, LC_MESSAGES, LANG
extern const char* const kControls[];   // noms des commandes de [touches], terminé par nullptr

// Lit le texte TOML ; les clés inconnues ou invalides gardent la valeur par défaut et ajoutent un message à `errors`.
Config Parse(const std::string& text, std::vector<std::string>* errors);
// Texte TOML documenté des valeurs de `c` (écrit au premier lancement).
std::string Serialize(const Config& c);
// <dossier>/config.toml : lu s'il existe, sinon créé avec les valeurs par défaut. Remplit `g`.
void Load(const std::string& dataDir);
// Fenêtre fermée : <dossier>/config.toml relu, écran, position et taille remplacés, fichier réécrit (Serialize).
// Fichier illisible : rien n'est écrit (faux).
bool SaveWindow(const std::string& dataDir, int screen, int x, int y, int w, int h);

}  // namespace wk::config
