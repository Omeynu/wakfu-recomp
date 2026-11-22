#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace wk {
// Écrit une image RGBA 8 bits (w*h*4 octets) ; false en cas d'erreur.
bool WritePng(const std::string& path, uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba);
}
