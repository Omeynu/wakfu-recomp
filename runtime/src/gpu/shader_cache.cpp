// Cache des shaders vus à l'exécution : analyse (Xenia), traduction GLSL, compilation SPIR-V.
// Bilan : « [shaders] » toutes les nouvelles traductions ; échecs détaillés une fois par shader.
#include <cstdlib>
#include "shader_cache.h"
#include <cstdio>
#include <map>
#include <filesystem>
#include <fstream>
#include <memory>
#define XXH_INLINE_ALL
#include <xxhash.h>
#include <mutex>
#include "glsl_translator.h"
#include "xenia/base/string_buffer.h"
#include "xenia/gpu/shader.h"

namespace wk::gpu {

namespace {
struct Entry {
    std::unique_ptr<xe::gpu::Shader> shader;
    bool ok = false;
    std::vector<uint32_t> spirv;
    std::string error;
};
// Jamais détruits : le thread de précompilation (CpPrecompileCatalog, détaché) peut encore s'en servir pendant la
// sortie du processus (destructeurs statiques).
std::mutex& g_mutex = *new std::mutex;
std::map<uint64_t, Entry>& g_cache = *new std::map<uint64_t, Entry>;
size_t g_ok = 0, g_fail = 0, g_fromDisk = 0;
std::string& g_dir = *new std::string;

// Cache SPIR-V : <dossier>/shaders/<XXH3 du GLSL et de l'étage>-<étage>.spv2 (SPIR-V + XXH3 du contenu). L'empreinte du GLSL change avec le traducteur :
// un cache périmé n'est jamais relu. Écriture par fichier temporaire + renommage.
std::string SpirvPath(const std::string& glsl, uint32_t type) {
    char name[40];
    snprintf(name, sizeof name, "/shaders/%016llx-%u.spv2", (unsigned long long)XXH3_64bits(glsl.data(), glsl.size()), type ? 1u : 0u);
    return g_dir + name;
}
bool LoadSpirv(const std::string& path, std::vector<uint32_t>* spirv) {
    // Fichier = SPIR-V puis XXH3 (8 octets) de ce SPIR-V : un fichier tronqué ou abîmé est recompilé, pas transmis au pilote.
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    const std::streamoff n = f ? std::streamoff(f.tellg()) - 8 : 0;
    if (n <= 0 || n % 4 || n > (16 << 20)) return false;
    spirv->resize(size_t(n) / 4);
    uint64_t sum = 0;
    f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(spirv->data()), n) || !f.read(reinterpret_cast<char*>(&sum), 8)) return false;
    return (*spirv)[0] == 0x07230203u && sum == XXH3_64bits(spirv->data(), size_t(n));   // nombre magique SPIR-V
}
void SaveSpirv(const std::string& path, const std::vector<uint32_t>& spirv) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        const uint64_t sum = XXH3_64bits(spirv.data(), spirv.size() * 4);
        f.write(reinterpret_cast<const char*>(spirv.data()), std::streamsize(spirv.size() * 4));
        f.write(reinterpret_cast<const char*>(&sum), 8);
        if (!f) return;
    }
    std::filesystem::rename(tmp, path, ec);
}
}  // namespace

void ShaderCacheSetDir(const std::string& dir) { g_dir = dir; }
const std::string& ShaderCacheDir() { return g_dir; }

void ShaderCacheOnLoad(uint32_t type, uint64_t hash, const uint8_t* ucode, uint32_t dwords) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_cache.count(hash)) return;
    Entry& e = g_cache[hash];
    e.shader = std::make_unique<xe::gpu::Shader>(type ? xe::gpu::xenos::ShaderType::kPixel : xe::gpu::xenos::ShaderType::kVertex,
                                                 hash, reinterpret_cast<const uint32_t*>(ucode), dwords);
    xe::StringBuffer buf;
    e.shader->AnalyzeUcode(buf);
    GlslTranslator tr;
    const std::string glsl = e.shader->is_ucode_analyzed() ? tr.Translate(*e.shader, &e.error) : "";
    if (!glsl.empty()) {
        const std::string path = g_dir.empty() ? "" : SpirvPath(glsl, type);
        if (!path.empty() && LoadSpirv(path, &e.spirv)) {
            e.ok = true;
            g_fromDisk++;
        } else {
            std::string log;
            e.ok = CompileGlsl(int(type ? 1 : 0), glsl, &e.spirv, &log);
            if (!e.ok) e.error = "compilation : " + log.substr(0, 200);
            else if (!path.empty()) SaveSpirv(path, e.spirv);
        }
    } else if (e.error.empty()) {
        e.error = "analyse impossible";
    }
    if (const char* dir = getenv("WAKFU_SHADER_DUMP")) {   // diagnostic : désassemblage + GLSL
        char name[32];
        snprintf(name, sizeof name, "/%016llx", (unsigned long long)hash);
        if (FILE* f = fopen((std::string(dir) + name + ".txt").c_str(), "w")) { fputs(buf.to_string().c_str(), f); fclose(f); }
        if (FILE* f = fopen((std::string(dir) + name + ".glsl").c_str(), "w")) { fputs(glsl.c_str(), f); fclose(f); }
    }
    (e.ok ? g_ok : g_fail)++;
    fprintf(stderr, "[shaders] %s %016llx (%s, %u mots) %s ; total : %zu traduits (%zu depuis le cache disque), %zu en échec\n",
            e.ok ? "traduit" : "ÉCHEC", (unsigned long long)hash, type ? "pixels" : "sommets", dwords, e.ok ? "" : e.error.c_str(),
            g_ok, g_fromDisk, g_fail);
}

ShaderCacheStats ShaderCacheGetStats() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return {g_ok, g_fail};
}

}  // namespace wk::gpu

namespace wk::gpu {
bool ShaderCacheGetSpirv(uint64_t hash, std::vector<uint32_t>* spirv, std::vector<uint32_t>* texturesUsed) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_cache.find(hash);
    if (it == g_cache.end() || !it->second.ok) return false;
    if (spirv) *spirv = it->second.spirv;
    texturesUsed->clear();
    for (auto& b : it->second.shader->texture_bindings()) texturesUsed->push_back(b.fetch_constant);
    return true;
}
}  // namespace wk::gpu
