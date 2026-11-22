// Analyse (et désassemblage) du microcode des shaders connus : fichiers .xpu/.xvu du jeu et
// conteneurs intégrés au XEX. Usage : wakfu_shaders <Media/Shaders> <image.bin> [dossier de sortie .txt]
#include <cstdio>
#include <filesystem>
#include <map>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>
#include "xenia/base/string_buffer.h"
#include "xenia/gpu/shader.h"
#include "gpu/glsl_translator.h"

namespace fs = std::filesystem;
using xe::gpu::Shader;
using xe::gpu::xenos::ShaderType;

struct Item { std::string name; ShaderType type; std::vector<uint8_t> ucode; };

static uint32_t Be(const std::vector<uint8_t>& b, size_t o) {
    return o + 4 <= b.size() ? uint32_t(b[o]) << 24 | uint32_t(b[o + 1]) << 16 | uint32_t(b[o + 2]) << 8 | b[o + 3] : 0;
}
// Conteneur : +0 drapeaux (bit 0 = vertex), +4 virtualSize, +0x18 shaderOffset ; Shader : +0 décalage, +4 taille
static bool Extract(const std::vector<uint8_t>& b, size_t o, const std::string& name, std::vector<Item>* out) {
    const uint32_t flags = Be(b, o), virt = Be(b, o + 4), phys = Be(b, o + 8), sh = Be(b, o + 0x18);
    if ((flags & 0xFFFFFF00u) != 0x102A1100u) return false;
    const uint32_t poff = Be(b, o + sh), size = Be(b, o + sh + 4);
    if (!size || size % 4 || o + virt + poff + size > b.size() || poff + size > phys) return false;
    out->push_back({name, (flags & 1) ? ShaderType::kVertex : ShaderType::kPixel,
                    std::vector<uint8_t>(b.begin() + o + virt + poff, b.begin() + o + virt + poff + size)});
    return true;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage : %s <Media/Shaders> <image.bin> [sortie]\n", argv[0]); return 2; }
    std::vector<Item> items;
    for (auto& e : fs::directory_iterator(argv[1])) {
        const std::string ext = e.path().extension().string();
        if (ext != ".xpu" && ext != ".xvu") continue;
        std::ifstream f(e.path(), std::ios::binary);
        std::vector<uint8_t> b{std::istreambuf_iterator<char>(f), {}};
        if (!Extract(b, 0, e.path().filename().string(), &items)) fprintf(stderr, "conteneur invalide : %s\n", e.path().c_str());
    }
    {
        std::ifstream f(argv[2], std::ios::binary);
        std::vector<uint8_t> b{std::istreambuf_iterator<char>(f), {}};
        for (size_t o = 0; o + 0x24 <= b.size(); o += 4) {
            if ((Be(b, o) & 0xFFFFFF00u) != 0x102A1100u || Be(b, o + 0x1C) || Be(b, o + 0x20)) continue;
            char name[32];
            snprintf(name, sizeof name, "xex_%08zX", size_t(0x82000000) + o);
            if (Extract(b, o, name, &items)) o += Be(b, o + 4) + Be(b, o + 8) - 4;
        }
    }
    const std::string outDir = argc > 3 ? argv[3] : "";
    if (!outDir.empty()) fs::create_directories(outDir);
    size_t ok = 0, vs = 0, ps = 0, textures = 0, vfetch = 0, translated = 0, compiled = 0;
    std::map<std::string, int> reasons;
    for (auto& it : items) {
        Shader s(it.type, 0, reinterpret_cast<const uint32_t*>(it.ucode.data()), it.ucode.size() / 4);
        xe::StringBuffer buf;
        s.AnalyzeUcode(buf);
        if (!s.is_ucode_analyzed()) { printf("ÉCHEC analyse : %s\n", it.name.c_str()); continue; }
        ok++;
        (it.type == ShaderType::kVertex ? vs : ps)++;
        textures += s.texture_bindings().size();
        vfetch += s.vertex_bindings().size();
        if (!outDir.empty()) std::ofstream(outDir + "/" + it.name + ".txt") << s.ucode_disassembly();
        wk::gpu::GlslTranslator tr;
        std::string err;
        const std::string glsl = tr.Translate(s, &err);
        if (glsl.empty()) { reasons["traduction : " + err]++; continue; }
        translated++;
        std::vector<uint32_t> spv;
        std::string log;
        if (!wk::gpu::CompileGlsl(it.type == ShaderType::kVertex ? 0 : 1, glsl, &spv, &log)) {
            reasons["compilation GLSL"]++;
            if (!outDir.empty()) {
                std::ofstream(outDir + "/" + it.name + ".glsl") << glsl;
                std::ofstream(outDir + "/" + it.name + ".log") << log;
            }
            continue;
        }
        compiled++;
        if (!outDir.empty()) std::ofstream(outDir + "/" + it.name + ".glsl") << glsl;
    }
    printf("shaders analysés : %zu/%zu (vertex %zu, pixel %zu) ; liaisons : %zu textures, %zu flux de sommets\n",
           ok, items.size(), vs, ps, textures, vfetch);
    printf("traduits en GLSL : %zu ; compilés en SPIR-V : %zu\n", translated, compiled);
    for (auto& [r, n] : reasons) printf("  échec (%d) : %s\n", n, r.c_str());
    return ok == items.size() && compiled == items.size() ? 0 : 1;
}
