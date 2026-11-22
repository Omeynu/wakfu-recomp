// Réglages de profil sur disque (cf. profile.h). X_USER_PROFILE_SETTING (40 octets, disposition de Xenia) :
// +0 source (0 non défini, 2 titre), +8 utilisateur (u32) ou XUID (u64), +0x10 identifiant, +0x18 type XUSER_DATA,
// +0x20 valeur (8 octets) ; chaîne (type 4) / binaire (type 6) : +0x20 taille, +0x24 adresse.
#include "profile.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>
#include "../guest.h"

namespace fs = std::filesystem;

namespace wk::profile {
namespace {
bool IsBlob(uint8_t type) { return type == 4 || type == 6; }
uint32_t MaxBlob(uint32_t id) { return (id >> 16) & 0xFFF; }   // taille maximale codée dans l'identifiant
std::string FileOf(const std::string& dir, uint32_t id) {
    char name[16];
    snprintf(name, sizeof name, "%08X.bin", id);
    return dir + "/" + name;
}
}  // namespace

uint32_t ReadSize(uint32_t count, uint32_t ids) {
    uint32_t data = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t id = Ld32(ids + 4 * i);
        if (IsBlob(uint8_t(id >> 28))) data += MaxBlob(id);
    }
    return 8 + 40 * count + data;
}

void Read(const std::string& dir, uint32_t count, uint32_t ids, uint32_t buf, uint32_t user, uint64_t xuid) {
    const uint32_t need = ReadSize(count, ids);
    for (uint32_t i = 0; i < need; i++) St8(buf + i, 0);
    St32(buf, count);
    St32(buf + 4, buf + 8);
    uint32_t data = buf + 8 + 40 * count;
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t e = buf + 8 + 40 * i, id = Ld32(ids + 4 * i);
        if (xuid) St64(e + 8, xuid); else St32(e + 8, user);
        St32(e + 0x10, id);
        const uint32_t slot = data;
        if (IsBlob(uint8_t(id >> 28))) data += MaxBlob(id);
        std::ifstream f(FileOf(dir, id), std::ios::binary);
        if (!f) continue;
        const std::vector<char> v((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (v.empty()) continue;
        const uint8_t type = uint8_t(v[0]);
        const uint32_t size = uint32_t(v.size() - 1);
        if (IsBlob(type)) {
            if (!IsBlob(uint8_t(id >> 28)) || size > MaxBlob(id)) continue;   // fichier incohérent : ignoré
            memcpy(Host(slot), v.data() + 1, size);
            St32(e + 0x20, size);
            St32(e + 0x24, slot);
        } else {
            if (size != 8) continue;
            memcpy(Host(e + 0x20), v.data() + 1, 8);
        }
        St32(e, 2);   // source : titre
        St8(e + 0x18, type);
    }
}

uint32_t Write(const std::string& dir, uint32_t count, uint32_t settings) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    uint32_t written = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t e = settings + 40 * i, id = Ld32(e + 0x10);
        uint8_t type = Ld8(e + 0x18);
        if (!type) type = uint8_t(id >> 28);   // le jeu laisse le type à 0 : celui codé dans l'identifiant
        std::vector<char> v(1, char(type));
        if (IsBlob(type)) {
            const uint32_t size = Ld32(e + 0x20), ptr = Ld32(e + 0x24);
            if (size > MaxBlob(id) || (size && !ptr)) continue;
            v.insert(v.end(), (const char*)Host(ptr), (const char*)Host(ptr) + size);
        } else {
            v.insert(v.end(), (const char*)Host(e + 0x20), (const char*)Host(e + 0x20) + 8);
        }
        // écriture dans un fichier temporaire puis renommage : pas de réglage tronqué si le jeu est coupé
        const std::string path = FileOf(dir, id), tmp = path + ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f.write(v.data(), std::streamsize(v.size()))) continue;
        }
        fs::rename(tmp, path, ec);
        if (!ec) written++;
    }
    return written;
}
}  // namespace wk::profile
