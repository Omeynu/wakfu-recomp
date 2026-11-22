#pragma once
#include <cstdint>
#include <string>

namespace wk {
struct FrontBuffer { uint32_t address = 0, width = 0, height = 0, pitch = 0, format = 0, endian = 0, tiled = 0; };
FrontBuffer ParseFetch(uint32_t fetchPtr);                         // constante de texture (6 mots) en mémoire
FrontBuffer ParseFetchWords(const uint32_t f[6]);
bool CaptureFrontBuffer(const uint32_t fetch[6], const std::string& path);   // constante de texture du tampon d'image
}
