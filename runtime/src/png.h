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
// Message court (UTF-8) en blanc sur un cadre sombre opaque, police 5x8 agrandie `echelle` fois : RGBA 8 bits, blanc et
// gris neutres (même octets en RGBA et BGRA). Caractères absents de la police dessinés en espace.
std::vector<uint8_t> TexteRgba(const std::string& utf8, uint32_t echelle, uint32_t* w, uint32_t* h);
}
