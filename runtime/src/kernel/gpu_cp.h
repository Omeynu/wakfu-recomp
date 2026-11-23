#pragma once
#include <cstdint>
#include <functional>

namespace wk {

// interrupt(source, cpu) : rappel invité à exécuter « sur » le CPU donné (PCR+0x10C)
// swap(numéro d'image) : traité dans l'ordre du flux GPU
struct CpCallbacks {
    std::function<void(uint32_t source, uint32_t cpu)> interrupt;
    std::function<void(uint32_t frame)> swap;
};
// Marqueur de swap écrit par VdSwap dans son emplacement de 64 mots (opcode 0x64, inutilisé par D3D)
constexpr uint32_t kCpSwapOpcode = 0x64, kCpSwapMagic = 0x57414B46;   // « WAKF »
void CpSetCallbacks(const CpCallbacks& cb);
void CpSetSwapCounter(uint32_t n);
// Exécute l'anneau de rptr à wptr (en mots) ; retourne le nouveau rptr.
uint32_t CpProcessRing(uint32_t ringBase, uint32_t sizeDwords, uint32_t rptr, uint32_t wptr);
uint32_t CpRegister(uint32_t index);   // valeur courante d'un registre GPU (tests)

}  // namespace wk
