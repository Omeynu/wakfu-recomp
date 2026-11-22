// FSR 1 : sources GLSL d'AMD intégrées (fsr1_glsl.h, produit par CMake depuis third_party/fidelityfx-fsr1) et
// constantes calculées par les fonctions CPU des mêmes en-têtes.
#include "fsr1.h"
#include <cmath>
#include <cstring>
#include "fsr1_glsl.h"   // kFfxAGlsl, kFfxFsr1Glsl
#define A_CPU 1
#include "ffx_a.h"
#define FSR_EASU_F 1
#define FSR_RCAS_F 1
#include "ffx_fsr1.h"

namespace wk::gpu {

namespace {
// Corps commun : 4 pixels par invocation (ordre ARmp8x8 d'AMD), bornes de l'image de sortie respectées.
const char* const kBody = R"(
layout(local_size_x = 64) in;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1, rgba8) uniform writeonly image2D dst;
layout(push_constant) uniform P { uvec4 c0, c1, c2, c3; } p;
#ifdef EASU
#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 q) { return AF4(textureGather(src, q, 0)); }
AF4 FsrEasuGF(AF2 q) { return AF4(textureGather(src, q, 1)); }
AF4 FsrEasuBF(AF2 q) { return AF4(textureGather(src, q, 2)); }
#else
#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 q) { return AF4(texelFetch(src, q, 0)); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#endif
)";
const char* const kMain = R"(
void Filter(AU2 q) {
    if (q.x >= uint(imageSize(dst).x) || q.y >= uint(imageSize(dst).y)) return;
    AF3 c;
#ifdef EASU
    FsrEasuF(c, q, p.c0, p.c1, p.c2, p.c3);
#else
    FsrRcasF(c.r, c.g, c.b, q, p.c0);
#endif
    imageStore(dst, ASU2(q), AF4(c, 1));
}
void main() {
    AU2 q = ARmp8x8(gl_LocalInvocationID.x) + AU2(gl_WorkGroupID.x << 4u, gl_WorkGroupID.y << 4u);
    Filter(q); q.x += 8u; Filter(q); q.y += 8u; Filter(q); q.x -= 8u; Filter(q);
}
)";
}  // namespace

std::string Fsr1Glsl(bool easu) {
    return std::string("#version 450\n") + (easu ? "#define EASU 1\n" : "") + "#define A_GPU 1\n#define A_GLSL 1\n" +
           kFfxAGlsl + kBody + kFfxFsr1Glsl + kMain;
}

void Fsr1EasuConstants(uint32_t out[16], uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH) {
    FsrEasuCon(out, out + 4, out + 8, out + 12, AF1(inW), AF1(inH), AF1(inW), AF1(inH), AF1(outW), AF1(outH));
}
void Fsr1RcasConstants(uint32_t out[16], float stops) {
    std::memset(out, 0, 16 * sizeof(uint32_t));
    FsrRcasCon(out, stops);
}

}  // namespace wk::gpu
