// Décodeur XMA : contextes matériels (XMA_CONTEXT_DATA, 64 octets gros-boutistes) et registres du bloc audio.
#pragma once
#include <cstdint>

namespace wk::xma {

constexpr uint32_t kContextCount = 320;
constexpr uint32_t kRegArrayBase = 0x7FEA1800;   // adresse physique du tableau de contextes (petit-boutiste)
constexpr uint32_t kRegKick = 0x7FEA1940;        // 10 registres de 32 bits (un bit par contexte) : activation
constexpr uint32_t kRegLock = 0x7FEA1A40;        // désactivation
constexpr uint32_t kRegClear = 0x7FEA1A80;       // remise à zéro

// Alloue le tableau des contextes en mémoire physique et écrit sa base dans le registre (au démarrage).
void Init();
// Adresse invitée (vue physique 0xA0000000) d'un contexte libre remis à zéro, 0 si tous sont pris.
uint32_t CreateContext();
void ReleaseContext(uint32_t guestAddr);
// Traite puis remet à zéro les registres clear, lock et kick (à appeler après une écriture du jeu).
// Un contexte activé est décodé tant que sa sortie a de la place et que son entrée a des paquets.
void ServiceRegisters();
// Bilan depuis le relevé précédent (journal) : contextes décodés au kick, et passages où la sortie était vide sans
// paquet d'entrée (le jeu n'a pas fourni la suite à temps, ou fin normale d'un son).
struct Bilan { uint32_t decodages = 0, aVide = 0; };
Bilan Releve();
// Vrai si le décodeur FFmpeg xma2 est disponible (sinon les tampons d'entrée sont consommés sans sortie).
bool HasDecoder();

}  // namespace wk::xma
