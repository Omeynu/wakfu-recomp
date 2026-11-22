// Extracteur STFS (paquets LIVE/PIRS/CON Xbox 360), portage de scripts/stfs_extract.py.
// Chaque bloc lu est vérifié contre son SHA-1 de la table de hachage de niveau 0.
#pragma once
#include <vector>
#include "common.h"

class Stfs
{
public:
    static constexpr uint32_t BLOCK = 0x1000, L0 = 0xAA, L1 = 0x70E4;
    struct Entry { std::string name; bool dir, consecutive; uint32_t blocks, start, size; int parent; };

    static bool probe(InputFile& f);          // magic LIVE / PIRS / « CON  »
    explicit Stfs(const fs::path& p);

    const std::vector<Entry>& entries() const { return entries_; }
    bool hasRootFile(const std::string& name) const;   // casse ignorée
    uint32_t contentType() const { return contentType_; }
    uint32_t titleId() const { return titleId_; }
    size_t extract(const fs::path& outdir);           // nombre de fichiers écrits

    // Adressage (formules Free60/Velocity, une table par niveau) ; publics pour les tests.
    static uint64_t dataBacking(uint32_t b);
    static uint64_t l0Backing(uint32_t b);
    uint64_t base() const { return base_; }
    uint32_t fileTableBlock() const { return ftBlock_; }

private:
    struct Hash { uint8_t sha[20]; uint32_t next; };
    Hash hashEntry(uint32_t b);
    void block(uint32_t b, uint8_t* out);
    void readEntries();

    InputFile f_;
    uint64_t base_ = 0;
    uint32_t contentType_ = 0, titleId_ = 0, ftBlock_ = 0, alloc_ = 0;
    uint16_t ftCount_ = 0;
    std::vector<Entry> entries_;
    uint64_t cachedTable_ = ~0ull;
    std::vector<uint8_t> table_;
};
