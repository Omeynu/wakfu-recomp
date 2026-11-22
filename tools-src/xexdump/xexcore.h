// Cœur de xexdump, partagé avec wakfu-prepare : applique un .xexp (optionnel), écrit le XEX
// patché et l'image chargée, liste sections et imports nommés.
#pragma once
#include <cstdint>
#include <filesystem>
#include <ostream>
#include <string>
#include <vector>

// patch vide : pas de TU (patchedXex est alors ignoré). Renvoie false et remplit err en cas d'échec.
bool xexPrepare(std::vector<uint8_t> xex, const std::vector<uint8_t>& patch,
                const std::filesystem::path& patchedXex, const std::filesystem::path& image,
                std::ostream& listing, std::string& err);

std::vector<uint8_t> readAllBytes(const std::filesystem::path& p, bool& ok);
bool writeAllBytes(const std::filesystem::path& p, const uint8_t* d, size_t n);
