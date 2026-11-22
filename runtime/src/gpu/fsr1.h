// AMD FidelityFX FSR 1 (third_party/fidelityfx-fsr1, MIT) : agrandissement EASU puis netteté RCAS de l'image
// affichée (option [post] agrandissement = "fsr1"). Les en-têtes d'AMD sont isolés dans fsr1.cpp (macros A_*).
#pragma once
#include <cstdint>
#include <string>

namespace wk::gpu {

// Source GLSL complète (#version compris) d'une passe de calcul : easu = true -> EASU, sinon RCAS.
// Liaisons : 0 = image source (sampler2D), 1 = image de sortie (rgba8) ; constantes poussées : 4 uvec4.
// Groupes de 64 invocations, 16x16 pixels par groupe.
std::string Fsr1Glsl(bool easu);
// Constantes poussées (16 mots) : EASU de inW x inH vers outW x outH ; RCAS (mots 0 à 3) de netteté `stops`
// (0 = maximale ; AMD recommande 0,2).
void Fsr1EasuConstants(uint32_t out[16], uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH);
void Fsr1RcasConstants(uint32_t out[16], float stops);

}  // namespace wk::gpu
