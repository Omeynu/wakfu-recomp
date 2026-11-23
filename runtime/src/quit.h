// Sortie du programme : code de sortie documenté (docs/options.md) et une ligne « [sortie] code N : raison »
// dans le journal, lisible par le lanceur. Implémenté dans main.cpp.
#pragma once

namespace wk {

enum ExitCode : int {
    kExitOk = 0,          // fenêtre fermée, le jeu quitte, « Quitter » à la manette
    kExitUsage = 2,       // arguments manquants
    kExitGameFiles = 3,   // image.bin ou default.xex absents ou invalides (recompiler)
    kExitMemory = 4,      // réservation des 4 Go de mémoire invitée impossible
    kExitGameError = 5,   // erreur fatale signalée par le jeu (KeBugCheck)
    kExitCrash = 6,       // plantage (exception du processeur hôte)
    kExitRender = 7,      // rendu natif impossible (GPU, Vulkan ou shaders précompilés) : message au joueur
};

// Écrit la ligne de sortie, vide les journaux et termine le processus sans destructeurs statiques (threads du jeu
// encore actifs).
[[noreturn]] void Quit(int code, const char* reason);

}  // namespace wk
