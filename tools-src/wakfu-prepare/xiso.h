// Extracteur XISO (système de fichiers XDVDFS des disques Xbox / Xbox 360).
// Image « XISO » reconstruite (partition à 0) ou image disque complète : la partition de jeu
// commence alors à un offset connu selon le format du disque (XGD1/XGD2/XGD3).
#pragma once
#include <set>
#include <vector>
#include "common.h"

class Xiso
{
public:
    static constexpr uint32_t SECTOR = 2048;
    static constexpr uint64_t VOLUME_SECTOR = 32;
    struct Layout { const char* name; uint64_t offset; };
    static constexpr Layout LAYOUTS[] = {
        { "XISO", 0 }, { "XGD3", 0x2080000 }, { "XGD2", 0xFD90000 }, { "XGD1", 0x18300000 } };
    static constexpr char MAGIC[] = "MICROSOFT*XBOX*MEDIA";   // 20 octets

    static const Layout* probe(InputFile& f);   // nullptr si aucun descripteur XDVDFS
    explicit Xiso(const fs::path& p);

    const Layout& layout() const { return *layout_; }
    bool hasRootFile(const std::string& name);   // casse ignorée
    size_t extract(const fs::path& outdir);      // nombre de fichiers écrits

private:
    struct Entry { std::string name; bool dir; uint32_t sector, size; };
    std::vector<Entry> readDir(uint32_t sector, uint32_t size);
    size_t extractDir(uint32_t sector, uint32_t size, const fs::path& out, int depth, NewPaths& np);

    InputFile f_;
    const Layout* layout_;
    uint32_t rootSector_ = 0, rootSize_ = 0;
    std::set<uint32_t> dirsSeen_;   // une table de dossier ne doit être visitée qu'une fois
};
