#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
namespace wk::gpu {
void ShaderCacheOnLoad(uint32_t type, uint64_t hash, const uint8_t* ucodeBigEndian, uint32_t dwords);
struct ShaderCacheStats { size_t translated = 0, failed = 0; };
ShaderCacheStats ShaderCacheGetStats();
// Dossier des caches sur disque (SPIR-V par empreinte du GLSL, pipelines Vulkan) ; vide : pas de cache disque.
void ShaderCacheSetDir(const std::string& dir);
const std::string& ShaderCacheDir();
// SPIR-V d'un shader traduit ; texturesUsed : emplacements de constantes de texture lus. false si absent ou en échec.
// spirv nul : seulement les emplacements de textures lus (pas de copie du SPIR-V)
bool ShaderCacheGetSpirv(uint64_t hash, std::vector<uint32_t>* spirv, std::vector<uint32_t>* texturesUsed);
}
