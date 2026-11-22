// Outils communs à l'extraction (STFS, XISO, dossier) : lecture positionnée 64 bits,
// noms de fichiers sûrs (aucune sortie du dossier cible, noms valides sous Windows).
#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

struct PrepareError : std::runtime_error { using std::runtime_error::runtime_error; };

std::string u8(const fs::path& p);
std::string lower(std::string s);

class InputFile
{
public:
    explicit InputFile(const fs::path& p);
    uint64_t size() const { return size_; }
    const fs::path& path() const { return path_; }
    bool tryRead(uint64_t off, void* dst, size_t n);
    void read(uint64_t off, void* dst, size_t n);   // PrepareError si hors du fichier ou erreur
private:
    fs::path path_;
    std::ifstream f_;
    uint64_t size_ = 0;
};

// Refuse vide, « . », « .. », séparateurs, caractères de contrôle ou interdits sous Windows,
// noms réservés (CON, NUL, COM1…), point ou espace final.
void checkName(const std::string& name);
// Chemins créés par une extraction : nom vérifié, refus des doublons (casse ignorée, comme sous
// Windows, pour un résultat identique sur les deux systèmes) et de tout fichier déjà présent.
class NewPaths
{
public:
    fs::path child(const fs::path& dir, const std::string& name);
private:
    std::set<std::string> seen_;
};
// Copie n octets de src (à partir de off) vers un nouveau fichier.
void copyRange(InputFile& src, uint64_t off, uint64_t n, const fs::path& dst);
