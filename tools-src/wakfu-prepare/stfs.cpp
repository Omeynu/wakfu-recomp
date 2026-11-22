#include "stfs.h"
#include <cstring>
#include <TinySHA1.hpp>

static uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint32_t le24(const uint8_t* p) { return p[0] | p[1] << 8 | uint32_t(p[2]) << 16; }

bool Stfs::probe(InputFile& f)
{
    char m[4];
    return f.tryRead(0, m, 4) && (!memcmp(m, "LIVE", 4) || !memcmp(m, "PIRS", 4) || !memcmp(m, "CON ", 4));
}

Stfs::Stfs(const fs::path& p) : f_(p)
{
    if (!probe(f_)) throw PrepareError(u8(p) + " : magic STFS inconnu");
    uint8_t h[0x379 + 0x24];
    f_.read(0, h, sizeof h);
    uint32_t headerSize = be32(h + 0x340);
    contentType_ = be32(h + 0x344);
    titleId_ = be32(h + 0x360);
    const uint8_t* vd = h + 0x379;
    if (vd[0] != 0x24) throw PrepareError(u8(p) + " : descripteur de volume STFS attendu (taille 0x24)");
    if (!(vd[2] & 1)) throw PrepareError(u8(p) + " : paquet « male » (2 tables par niveau) non géré");
    ftCount_ = uint16_t(vd[3] | vd[4] << 8);
    ftBlock_ = le24(vd + 5);
    alloc_ = be32(vd + 0x1C);
    base_ = (uint64_t(headerSize) + 0xFFF) & ~uint64_t(0xFFF);
    readEntries();
}

uint64_t Stfs::dataBacking(uint32_t b)
{
    uint64_t r = (uint64_t(b) + L0) / L0 + b;
    if (b < L0) return r;
    r += (uint64_t(b) + L1) / L1;
    return b < L1 ? r : r + 1;
}

uint64_t Stfs::l0Backing(uint32_t b)
{
    if (b < L0) return 0;
    uint64_t n = uint64_t(b / L0) * (L0 + 1) + b / L1 + 1;
    return b < L1 ? n : n + 1;
}

Stfs::Hash Stfs::hashEntry(uint32_t b)
{
    uint64_t t = base_ + l0Backing(b) * BLOCK;
    if (t != cachedTable_)
    {
        table_.resize(BLOCK);
        f_.read(t, table_.data(), BLOCK);
        cachedTable_ = t;
    }
    const uint8_t* e = table_.data() + (b % L0) * 0x18;
    Hash h;
    memcpy(h.sha, e, 20);
    h.next = uint32_t(e[0x15]) << 16 | e[0x16] << 8 | e[0x17];
    return h;
}

void Stfs::block(uint32_t b, uint8_t* out)
{
    if (b >= alloc_) throw PrepareError("STFS : bloc " + std::to_string(b) + " hors du paquet (" + std::to_string(alloc_) + " alloués)");
    f_.read(base_ + dataBacking(b) * BLOCK, out, BLOCK);
    sha1::SHA1 s;
    s.processBytes(out, BLOCK);
    uint8_t d[20];
    s.finalize(d);
    if (memcmp(d, hashEntry(b).sha, 20)) throw PrepareError("STFS : SHA-1 incorrect pour le bloc " + std::to_string(b) + " (paquet abîmé)");
}

void Stfs::readEntries()
{
    uint8_t data[BLOCK];
    uint32_t b = ftBlock_;
    for (uint32_t i = 0; i < ftCount_; ++i, b = hashEntry(b).next)
    {
        block(b, data);
        for (uint32_t o = 0; o < BLOCK; o += 0x40)
        {
            const uint8_t* e = data + o;
            uint8_t flags = e[0x28];
            uint32_t n = flags & 0x3F;
            if (n == 0) return;
            if (n > 0x28) throw PrepareError("STFS : nom trop long dans la table des fichiers");
            Entry en;
            en.name.assign(reinterpret_cast<const char*>(e), n);
            en.dir = flags & 0x80;
            en.consecutive = flags & 0x40;
            en.blocks = le24(e + 0x29);
            en.start = le24(e + 0x2F);
            en.parent = int16_t(e[0x32] << 8 | e[0x33]);
            en.size = be32(e + 0x34);
            entries_.push_back(std::move(en));
        }
    }
}

bool Stfs::hasRootFile(const std::string& name) const
{
    for (auto& e : entries_)
        if (!e.dir && e.parent == -1 && lower(e.name) == lower(name)) return true;
    return false;
}

size_t Stfs::extract(const fs::path& outdir)
{
    // Chemin de chaque entrée (indices de parents vérifiés, profondeur bornée), créé à la demande.
    std::vector<fs::path> paths(entries_.size());
    std::vector<int> state(entries_.size(), 0);   // 0 inconnu, 1 en cours, 2 fait
    NewPaths np;
    auto pathOf = [&](int i, auto& self, int depth = 0) -> const fs::path& {
        if (depth > 64) throw PrepareError("STFS : arborescence trop profonde");
        if (i < 0 || size_t(i) >= entries_.size()) throw PrepareError("STFS : parent invalide dans la table des fichiers");
        if (state[i] == 2) return paths[i];
        if (state[i] == 1 || !entries_[i].dir) throw PrepareError("STFS : arborescence invalide (boucle ou parent non dossier)");
        state[i] = 1;
        fs::path parent = entries_[i].parent == -1 ? outdir : self(entries_[i].parent, self, depth + 1);
        paths[i] = np.child(parent, entries_[i].name);
        fs::create_directory(paths[i]);
        state[i] = 2;
        return paths[i];
    };
    std::vector<uint8_t> data(BLOCK);
    size_t files = 0;
    for (size_t i = 0; i < entries_.size(); ++i)
    {
        const Entry& e = entries_[i];
        if (e.dir) { pathOf(int(i), pathOf); continue; }
        fs::path dir = e.parent == -1 ? outdir : pathOf(e.parent, pathOf);
        fs::path p = np.child(dir, e.name);
        uint32_t need = uint32_t((uint64_t(e.size) + BLOCK - 1) / BLOCK);
        if (need > e.blocks) throw PrepareError("STFS : " + u8(p) + " : taille supérieure aux blocs alloués");
        std::ofstream o(p, std::ios::binary);
        if (!o) throw PrepareError("création impossible : " + u8(p));
        uint32_t left = e.size, b = e.start;
        for (uint32_t k = 0; k < need; ++k)
        {
            block(b, data.data());
            uint32_t n = left < BLOCK ? left : BLOCK;
            o.write(reinterpret_cast<const char*>(data.data()), n);
            left -= n;
            if (k + 1 < need) b = e.consecutive ? e.start + k + 1 : hashEntry(b).next;
        }
        o.close();
        if (o.fail()) throw PrepareError("écriture impossible : " + u8(p));
        ++files;
    }
    return files;
}
