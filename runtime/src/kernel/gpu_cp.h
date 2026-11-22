#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace wk {

// interrupt(source, cpu) : rappel invité à exécuter « sur » le CPU donné (PCR+0x10C)
// swap(numéro d'image, constante de texture du tampon d'image) : traité dans l'ordre du flux GPU
struct CpCallbacks {
    std::function<void(uint32_t source, uint32_t cpu)> interrupt;
    std::function<void(uint32_t frame, const uint32_t fetch[6])> swap;
};
// Marqueur de swap écrit par VdSwap dans son emplacement de 64 mots (opcode 0x64, inutilisé par D3D)
constexpr uint32_t kCpSwapOpcode = 0x64, kCpSwapMagic = 0x57414B46;   // « WAKF »
void CpSetCallbacks(const CpCallbacks& cb);
void CpSetSwapCounter(uint32_t n);
// Exécute l'anneau de rptr à wptr (en mots) ; retourne le nouveau rptr.
uint32_t CpProcessRing(uint32_t ringBase, uint32_t sizeDwords, uint32_t rptr, uint32_t wptr);
void CpDumpStats();
uint32_t CpRegister(uint32_t index);   // valeur courante d'un registre GPU (tests)

// Jalon 3 : état Xenos et statistiques par image
struct CpResolve { uint32_t destBase = 0, destPitch = 0, destInfo = 0, surfaceInfo = 0, colorInfo = 0; };
struct CpFrameStats {
    uint32_t draws = 0, resolves = 0, indices = 0, shaderLoads = 0, shaders = 0, unknownShaders = 0;
    uint32_t totalShaders = 0, totalUnknownShaders = 0, totalXexShaders = 0;   // xex : intégrés au XEX
    uint64_t totalResolves = 0;
    std::string prims, unknownOps;   // « type:nombre » ; paquets type 3 inconnus « opcode:nombre » (cumulés)
    CpResolve lastResolve;
};
// Rappel à chaque chargement de microcode (type 0 sommets / 1 pixels, hachage, mots gros-boutistes).
using CpShaderHook = std::function<void(uint32_t type, uint64_t hash, const uint8_t* ucodeBigEndian, uint32_t dwords)>;
void CpSetShaderHook(CpShaderHook hook);
void CpLoadShaderCatalog(const std::string& dir);   // .xpu/.xvu du jeu
// Traduit d'avance, sur un thread à part, les shaders du jeu dont l'étage est connu (.xvu sommets, .xpu pixels) :
// plus de compilation au premier affichage (saccades).
void CpPrecompileCatalog(CpShaderHook hook);
void CpLoadShaderCatalogFromImage(uint32_t base, uint32_t size);   // conteneurs intégrés au XEX
CpFrameStats CpEndFrame();                         // à l'appel de VdSwap : bilan de l'image puis remise à zéro

}  // namespace wk
