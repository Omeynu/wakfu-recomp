#include "xiso.h"
#include <cstring>

static uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }
static uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

const Xiso::Layout* Xiso::probe(InputFile& f)
{
    for (auto& l : LAYOUTS)
    {
        char m[20];
        if (f.tryRead(l.offset + VOLUME_SECTOR * SECTOR, m, 20) && !memcmp(m, MAGIC, 20)) return &l;
    }
    return nullptr;
}

Xiso::Xiso(const fs::path& p) : f_(p), layout_(probe(f_))
{
    if (!layout_) throw PrepareError(u8(p) + " : aucun système de fichiers XDVDFS trouvé");
    uint8_t vd[28];
    f_.read(layout_->offset + VOLUME_SECTOR * SECTOR, vd, sizeof vd);
    rootSector_ = le32(vd + 20);
    rootSize_ = le32(vd + 24);
}

std::vector<Xiso::Entry> Xiso::readDir(uint32_t sector, uint32_t size)
{
    std::vector<Entry> out;
    if (size == 0) return out;                         // dossier vide
    if (size > 64u << 20) throw PrepareError("XISO : table de dossier démesurée");
    std::vector<uint8_t> t(size);
    f_.read(layout_->offset + uint64_t(sector) * SECTOR, t.data(), size);
    // Arbre binaire : chaque nœud donne les offsets (en mots de 4 octets) de ses fils gauche/droit.
    std::vector<uint32_t> todo{ 0 };
    std::vector<bool> seen(size / 4 + 1, false);
    while (!todo.empty())
    {
        uint32_t off = todo.back();
        todo.pop_back();
        if (off + 14 > size) throw PrepareError("XISO : entrée hors de la table de dossier");
        if (seen[off / 4]) throw PrepareError("XISO : boucle dans la table de dossier");
        seen[off / 4] = true;
        const uint8_t* e = t.data() + off;
        uint16_t left = le16(e), right = le16(e + 2);
        if (left == 0xFFFF && right == 0xFFFF) { if (off == 0) break; throw PrepareError("XISO : entrée de remplissage référencée"); }
        uint8_t attr = e[12], n = e[13];
        if (n == 0 || off + 14 + n > size) throw PrepareError("XISO : nom d'entrée invalide");
        out.push_back({ std::string(reinterpret_cast<const char*>(e + 14), n), bool(attr & 0x10), le32(e + 4), le32(e + 8) });
        if (left) todo.push_back(uint32_t(left) * 4);
        if (right) todo.push_back(uint32_t(right) * 4);
    }
    return out;
}

bool Xiso::hasRootFile(const std::string& name)
{
    for (auto& e : readDir(rootSector_, rootSize_))
        if (!e.dir && lower(e.name) == lower(name)) return true;
    return false;
}

size_t Xiso::extractDir(uint32_t sector, uint32_t size, const fs::path& out, int depth, NewPaths& np)
{
    if (depth > 64) throw PrepareError("XISO : arborescence trop profonde");
    if (size && !dirsSeen_.insert(sector).second) throw PrepareError("XISO : table de dossier partagée ou en boucle");
    size_t files = 0;
    for (auto& e : readDir(sector, size))
    {
        fs::path p = np.child(out, e.name);
        if (e.dir)
        {
            fs::create_directory(p);
            files += extractDir(e.sector, e.size, p, depth + 1, np);
        }
        else
        {
            copyRange(f_, layout_->offset + uint64_t(e.sector) * SECTOR, e.size, p);
            ++files;
        }
    }
    return files;
}

size_t Xiso::extract(const fs::path& outdir) { NewPaths np; return extractDir(rootSector_, rootSize_, outdir, 0, np); }
