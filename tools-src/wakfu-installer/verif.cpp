// Vérification rapide d'un fichier ou dossier choisi (fenêtre et ligne de commande), avec les lecteurs STFS et XISO de
// wakfu-prepare : en-tête, taille, table des fichiers (blocs vérifiés par leur SHA-1). Le contrôle complet (application
// de la TU, SHA-1 de l'image) reste celui de wakfu-prepare, à l'étape de préparation (code de sortie 3).
#include <cstring>
#include "installer.h"
#include "stfs.h"
#include "xiso.h"

namespace wk {

namespace {

constexpr uint32_t kTitreWakfu = 0x584109AA;   // Islands of Wakfu (XBLA), jeu et mise à jour
// Empreinte de la table de hachage racine du paquet de la Title Update 2 (descripteur de volume STFS) : le paquet,
// signé par Microsoft, est le même pour tous (TU_1C422DA_0000004000000.00000000000G1). Toute autre valeur : autre TU.
constexpr uint8_t kRacineTu2[20] = { 0xdd, 0xfa, 0x4b, 0xa0, 0x4e, 0x61, 0xa0, 0xd4, 0xa4, 0x8a,
                                     0x02, 0x19, 0x81, 0x21, 0x37, 0xc8, 0x65, 0x1f, 0xe2, 0xd1 };

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

// Nom affiché du paquet (en-tête STFS, UTF-16 grand-boutiste, 0x80 octets) en UTF-8, caractères de contrôle retirés
std::string nomAffiche(const uint8_t* p)
{
    std::string r;
    for (int i = 0; i + 1 < 0x80; i += 2)
    {
        uint32_t c = uint32_t(p[i]) << 8 | p[i + 1];
        if (c == 0) break;
        if (c >= 0xD800 && c <= 0xDFFF) c = 0xFFFD;   // paires de substitution : rares dans un nom, remplacées
        if (c < 0x20) continue;
        if (c < 0x80) r += char(c);
        else if (c < 0x800) r += char(0xC0 | c >> 6), r += char(0x80 | (c & 0x3F));
        else r += char(0xE0 | c >> 12), r += char(0x80 | ((c >> 6) & 0x3F)), r += char(0x80 | (c & 0x3F));
    }
    return r;
}

bool fichierRacine(const fs::path& dir, const char* nom)   // casse ignorée, comme wakfu-prepare
{
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec) && lower(u8(e.path().filename())) == nom) return true;
    return false;
}

} // namespace

Paquet verifierPaquet(const fs::path& p, InfoPaquet* info)
{
    *info = {};
    std::error_code ec;
    if (fs::is_directory(p, ec))
    {
        info->format = "dossier";
        bool xex = fichierRacine(p, "default.xex"), xexp = fichierRacine(p, "default.xexp");
        if (xex && xexp) return Paquet::DossierMixte;
        return xex ? Paquet::Jeu : xexp ? Paquet::MiseAJour : Paquet::Dossier;
    }
    try
    {
        InputFile f(p);
        if (!Stfs::probe(f))
        {
            if (!Xiso::probe(f)) return Paquet::NonStfs;
            info->format = "image disque";
            Xiso x(p);   // wakfu-prepare n'accepte une image que pour le jeu
            return x.hasRootFile("default.xex") ? Paquet::Jeu : Paquet::NiJeuNiMaj;
        }
        info->format = "paquet STFS";
        uint8_t h[0x500];
        if (!f.tryRead(0, h, sizeof h)) return Paquet::Tronque;
        info->titre = be32(h + 0x360);
        info->nom = nomAffiche(h + 0x411);
        if (info->titre != kTitreWakfu) return Paquet::AutreJeu;
        // Taille attendue : jusqu'au dernier bloc alloué (descripteur de volume), tables de hachage comprises
        const uint8_t* vd = h + 0x379;
        uint32_t alloues = be32(vd + 0x1C);
        uint64_t base = (uint64_t(be32(h + 0x340)) + 0xFFF) & ~uint64_t(0xFFF);
        if (alloues && f.size() < base + (Stfs::dataBacking(alloues - 1) + 1) * Stfs::BLOCK) return Paquet::Tronque;
        Stfs s(p);
        if (s.hasRootFile("default.xex")) return Paquet::Jeu;
        if (s.hasRootFile("default.xexp"))
            return std::memcmp(vd + 8, kRacineTu2, 20) == 0 ? Paquet::MiseAJour : Paquet::MajNonPriseEnCharge;
        return Paquet::NiJeuNiMaj;
    }
    catch (const std::exception&)
    {
        return Paquet::Illisible;
    }
}

} // namespace wk
