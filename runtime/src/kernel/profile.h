// Réglages de profil XAM (XamUserRead/WriteProfileSettings) conservés sur disque : un fichier par réglage,
// <dossier>/<id en hexadécimal>.bin = octet de type XUSER_DATA puis valeur (8 octets, ou contenu d'une chaîne /
// d'un binaire). Fonctions sur la mémoire invitée, sans import, pour le test unitaire.
#pragma once
#include <cstdint>
#include <string>

namespace wk::profile {
// Taille du tampon de réponse pour ces identifiants (en-tête 8 + 40 par réglage + données chaîne/binaire).
uint32_t ReadSize(uint32_t count, uint32_t ids);
// Remplit le tampon (taille ReadSize) : réglages connus du dossier « source titre », sinon « non définis ».
// xuid != 0 : entrées par XUID, sinon par numéro d'utilisateur.
void Read(const std::string& dir, uint32_t count, uint32_t ids, uint32_t buf, uint32_t user, uint64_t xuid);
// Écrit les réglages (tableau de X_USER_PROFILE_SETTING de 40 octets). Renvoie le nombre de réglages écrits.
uint32_t Write(const std::string& dir, uint32_t count, uint32_t settings);
}  // namespace wk::profile
