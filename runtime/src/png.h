#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace wk {
// Écrit une image RGBA 8 bits (w*h*4 octets) ; false en cas d'erreur.
bool WritePng(const std::string& path, uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba);
// Icône Windows (.ico) qui contient le PNG tel quel (format reconnu depuis Vista). false si ce n'est pas un PNG
// de 1 à 256 pixels de côté ou en cas d'erreur d'écriture.
bool PngVersIco(const std::string& png, const std::string& ico);
}
