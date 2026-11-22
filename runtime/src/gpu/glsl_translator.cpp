// Traducteur microcode Xenos -> GLSL. Sémantique des opérations : commentaires de ucode.h (Xenia),
// règle « 0 × n'importe quoi = 0 » des multiplications Xenos (comme Xenia).
#include "glsl_translator.h"
#include <cstdio>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

namespace wk::gpu {

using namespace xe::gpu;
using ucode::AluScalarOpcode;
using ucode::AluVectorOpcode;

namespace {
const char* kPrelude = R"(#version 450
layout(std140, set = 0, binding = 0) uniform Consts { vec4 c[512]; uvec4 b[2]; uvec4 l[8]; } K;
layout(std430, set = 0, binding = 1) readonly buffer Mem { uint mem[]; } M[4];   // 4 tranches de 128 Mo
layout(std140, set = 0, binding = 2) uniform Fetch { uvec4 f[48]; } F;
layout(set = 0, binding = 3) uniform sampler2D tex[32];
layout(push_constant) uniform Push { vec4 ndcScale; vec4 ndcOffset; int vertexIndexOffset; uint vte; uint alphaFunc; float alphaRef;
                                     uint indexByte; uint indexMode; } P;   // indices lus ici (indexMode : taille, mode d'octets, quads)

uint xfetch(uint i) { return F.f[i >> 2][i & 3u]; }
bool xbool(uint i) { return ((K.b[i >> 7][(i >> 5) & 3u] >> (i & 31u)) & 1u) != 0u; }
uint xloop(uint i) { return K.l[i >> 2][i & 3u]; }
// Le GPU lit la mémoire en petit-boutiste ; mode d'octets : 1 = 8 dans 16, 2 = 8 dans 32, 3 = 16 dans 32
uint xswap(uint v, uint e) {
    if (e == 1u) return ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
    if (e == 2u) return (v << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24);
    if (e == 3u) return (v << 16) | (v >> 16);
    return v;
}
uint xread(uint dw, uint e) { dw &= 0x7FFFFFFu; return xswap(M[dw >> 25].mem[dw & 0x1FFFFFFu], e); }   // 512 Mo physiques
// Multiplications Xenos : 0 × n'importe quoi = 0 (y compris ∞ et NaN)
float xmul(float a, float b) { return (a == 0.0 || b == 0.0) ? 0.0 : a * b; }
vec4 xmul(vec4 a, vec4 b) { return vec4(xmul(a.x, b.x), xmul(a.y, b.y), xmul(a.z, b.z), xmul(a.w, b.w)); }
float xdot4(vec4 a, vec4 b) { return xmul(a.x, b.x) + xmul(a.y, b.y) + xmul(a.z, b.z) + xmul(a.w, b.w); }
float xdot3(vec4 a, vec4 b) { return xmul(a.x, b.x) + xmul(a.y, b.y) + xmul(a.z, b.z); }
float xclampmax(float t) { return isinf(t) ? (t < 0.0 ? -3.402823466e38 : 3.402823466e38) : t; }
float xclampzero(float t) { return isinf(t) ? (t < 0.0 ? -0.0 : 0.0) : t; }
float xsigned(uint v, uint bits) { return float(int(v << (32u - bits)) >> int(32u - bits)); }
)";

std::string F(float v) {
    char b[32];
    snprintf(b, sizeof b, "%.9g", v);
    std::string s = b;
    if (s.find_first_of(".einn") == std::string::npos) s += ".0";
    return s;
}
std::string U(uint32_t v) { return std::to_string(v) + "u"; }
}  // namespace

void GlslTranslator::Line(const std::string& s) {
    body_.append(size_t(indent_) * 4, ' ');
    body_ += s;
    body_ += '\n';
}

void GlslTranslator::Reset() {
    ShaderTranslator::Reset();
    body_.clear();
    errors_.clear();
    indent_ = 0;
    usesTextures_ = false;
    lastVfetchConstant_ = lastVfetchStride_ = 0;
    lastVfetchIndexExpr_ = "0.0";
}

void GlslTranslator::EmitTranslationError(const char* message, bool is_fatal) {
    ShaderTranslator::EmitTranslationError(message, is_fatal);
    errors_.push_back(message);
}

std::string GlslTranslator::Translate(Shader& shader, std::string* error) {
    Shader::Translation* t = shader.GetOrCreateTranslation(0);
    if (!TranslateAnalyzedShader(*t) || !errors_.empty()) {
        if (error) {
            *error = errors_.empty() ? "traduction refusée" : errors_.front();
            for (size_t i = 1; i < errors_.size() && i < 4; i++) *error += " ; " + errors_[i];
        }
        return "";
    }
    const auto& bin = t->translated_binary();
    return std::string(bin.begin(), bin.end());
}

void GlslTranslator::StartTranslation() {
    indent_ = 2;
}

std::vector<uint8_t> GlslTranslator::CompleteTranslation() {
    std::string s = kPrelude;
    const uint32_t regs = register_count() ? register_count() : 1;
    if (is_vertex_shader()) {
        s += "out gl_PerVertex { vec4 gl_Position; float gl_PointSize; };\nlayout(location = 0) out vec4 xo[16];\n";
    } else {
        s += "layout(location = 0) in vec4 xi[16];\nlayout(location = 0) out vec4 oC[4];\n";
    }
    s += "void main() {\n";
    s += "    vec4 r[" + std::to_string(regs) + "];\n";
    s += "    for (int i = 0; i < " + std::to_string(regs) + "; i++) r[i] = vec4(0.0);\n";
    if (is_vertex_shader()) {
        // r0.x = indice du sommet (en flottant, comme le VGT de Xenos)
        // indexMode : bits 0-1 taille (0 aucun, 1 16 bits, 2 32 bits), 4-5 mode d'octets, 8 quadrilatères -> triangles
        s += "    uint xvid = uint(gl_VertexIndex);\n";
        s += "    if ((P.indexMode & 0x100u) != 0u) xvid = (xvid / 6u) * 4u + ((0x320210u >> ((xvid % 6u) * 4u)) & 0xFu);\n";
        s += "    if ((P.indexMode & 3u) != 0u) {\n";
        s += "        uint xb = P.indexByte + xvid * ((P.indexMode & 3u) == 2u ? 4u : 2u);\n";
        s += "        uint xw = xread(xb >> 2, (P.indexMode >> 4) & 3u);\n";
        s += "        xvid = (P.indexMode & 3u) == 2u ? xw : (((xb >> 1) & 1u) != 0u ? xw >> 16 : xw & 0xFFFFu);\n";
        s += "    }\n";
        s += "    r[0].x = float(int(xvid) + P.vertexIndexOffset);\n";
        s += "    for (int i = 0; i < 16; i++) xo[i] = vec4(0.0);\n";
        s += "    vec4 xpos = vec4(0.0, 0.0, 0.0, 1.0);\n";
    } else {
        s += "    for (int i = 0; i < " + std::to_string(regs < 16 ? regs : 16) + "; i++) r[i] = xi[i];\n";
        s += "    for (int i = 0; i < 4; i++) oC[i] = vec4(0.0);\n";
    }
    s += "    float ps = 0.0; bool p0 = false; int a0 = 0;\n";
    s += "    int aL = 0; int aLs[4]; int lc[4]; int ld = 0;\n";
    s += "    vec4 xv; float xs; vec4 xt;\n";
    s += "    int pc = 0;\n    for (int guard = 0; guard < 65536; guard++) {\n      switch (pc) {\n";
    s += "        case 0:\n";
    s += body_;
    s += "        default: guard = 65536; break;\n      }\n    }\n";
    if (is_pixel_shader()) {
        // Test alpha (RB_COLORCONTROL / RB_ALPHA_REF) ; fonctions Xenos : jamais, <, =, <=, >, !=, >=, toujours
        s += "    if (P.alphaFunc != 7u) {\n      float a = oC[0].a; bool ok;\n      switch (P.alphaFunc) {\n"
             "        case 0u: ok = false; break; case 1u: ok = a < P.alphaRef; break; case 2u: ok = a == P.alphaRef; break;\n"
             "        case 3u: ok = a <= P.alphaRef; break; case 4u: ok = a > P.alphaRef; break; case 5u: ok = a != P.alphaRef; break;\n"
             "        default: ok = a >= P.alphaRef; break;\n      }\n      if (!ok) discard;\n    }\n";
    }
    if (is_vertex_shader()) {
        // Transformation vers le NDC Vulkan (échelle/décalage calculés par le runtime ; y vers le bas)
        // PA_CL_VTE_CNTL (cf. Xenia) : bit 10 = W fourni tel quel (sinon 1/W), bits 8/9 = XY / Z déjà divisés par W
        s += "    if ((P.vte & 0x400u) == 0u) xpos.w = 1.0 / xpos.w;\n";
        s += "    if ((P.vte & 0x100u) != 0u) xpos.xy *= xpos.w;\n";
        s += "    if ((P.vte & 0x200u) != 0u) xpos.z *= xpos.w;\n";
        s += "    gl_Position = vec4(xpos.xyz * P.ndcScale.xyz + P.ndcOffset.xyz * xpos.w, xpos.w);\n";
        s += "    gl_PointSize = 1.0;\n";
    }
    s += "}\n";
    return std::vector<uint8_t>(s.begin(), s.end());
}

// ---------- Flot de contrôle ----------
std::string GlslTranslator::Condition(bool isBool, uint32_t boolIndex, bool condition) const {
    if (isBool) return std::string(condition ? "" : "!") + "xbool(" + U(boolIndex) + ")";
    return condition ? "p0" : "!p0";
}

void GlslTranslator::ProcessLabel(uint32_t cf_index) {
    if (cf_index == 0) return;   // case 0 émis d'office
    body_ += "        case " + std::to_string(cf_index) + ":\n";
}

void GlslTranslator::ProcessExecInstructionBegin(const ParsedExecInstruction& i) {
    switch (i.type) {
        case ParsedExecInstruction::Type::kConditional:
            Line("if (" + Condition(true, i.bool_constant_index, i.condition) + ") {");
            break;
        case ParsedExecInstruction::Type::kPredicated:
            Line("if (" + Condition(false, 0, i.condition) + ") {");
            break;
        default:
            Line("{");
    }
    indent_++;
}

void GlslTranslator::ProcessExecInstructionEnd(const ParsedExecInstruction& i) {
    if (i.is_end) Line("guard = 65536; break;");   // fin du programme
    indent_--;
    Line("}");
}

void GlslTranslator::ProcessLoopStartInstruction(const ParsedLoopStartInstruction& i) {
    // compte bits 0:7, aL initial bits 8:15
    Line("{ uint lv = xloop(" + U(i.loop_constant_index) + "); if (ld < 4) { aLs[ld] = aL; lc[ld] = int(lv & 0xFFu); ld++; }");
    if (!i.is_repeat) Line("  aL = int((lv >> 8) & 0xFFu);");
    Line("  if (lc[ld - 1] == 0) { ld--; aL = aLs[ld]; pc = " + std::to_string(i.loop_skip_address) + "; break; } }");
}

void GlslTranslator::ProcessLoopEndInstruction(const ParsedLoopEndInstruction& i) {
    // pas signé bits 16:23
    Line("{ uint lv = xloop(" + U(i.loop_constant_index) + "); lc[ld - 1]--; aL += int(lv << 8) >> 24;");
    std::string brk = i.is_predicated_break ? " && !(" + Condition(false, 0, i.predicate_condition) + ")" : "";
    Line("  if (lc[ld - 1] > 0" + brk + ") { pc = " + std::to_string(i.loop_body_address) + "; break; }");
    Line("  ld--; aL = aLs[ld]; }");
}

void GlslTranslator::ProcessCallInstruction(const ParsedCallInstruction&) {
    EmitTranslationError("appel de sous-programme (call) non géré", true);
}
void GlslTranslator::ProcessReturnInstruction(const ParsedReturnInstruction&) {
    EmitTranslationError("retour de sous-programme (ret) non géré", true);
}

void GlslTranslator::ProcessJumpInstruction(const ParsedJumpInstruction& i) {
    const std::string go = "{ pc = " + std::to_string(i.target_address) + "; break; }";
    switch (i.type) {
        case ParsedJumpInstruction::Type::kConditional:
            Line("if (" + Condition(true, i.bool_constant_index, i.condition) + ") " + go); break;
        case ParsedJumpInstruction::Type::kPredicated:
            Line("if (" + Condition(false, 0, i.condition) + ") " + go); break;
        default:
            Line(go);
    }
}

// ---------- Opérandes et résultats ----------
std::string GlslTranslator::RegisterIndex(uint32_t index, InstructionStorageAddressingMode mode) const {
    switch (mode) {
        case InstructionStorageAddressingMode::kAddressRegisterRelative: return std::to_string(index) + " + a0";
        case InstructionStorageAddressingMode::kLoopRelative: return std::to_string(index) + " + aL";
        default: return std::to_string(index);
    }
}

std::string GlslTranslator::Operand(const InstructionOperand& op) const {
    std::string base;
    switch (op.storage_source) {
        case InstructionStorageSource::kRegister:
            base = "r[" + RegisterIndex(op.storage_index, op.storage_addressing_mode) + "]";
            break;
        case InstructionStorageSource::kConstantFloat:
            base = "K.c[" + std::string(is_pixel_shader() ? "256 + " : "") +
                   RegisterIndex(op.storage_index, op.storage_addressing_mode) + "]";
            break;
        default:
            base = "vec4(0.0)";
    }
    if (op.is_absolute_value) base = "abs(" + base + ")";
    if (op.is_negated) base = "(-" + base + ")";
    static const char* kComp[] = {"x", "y", "z", "w"};
    std::string parts[4];
    bool identity = true;
    for (uint32_t i = 0; i < 4; i++) {
        const SwizzleSource s = op.GetComponent(i);
        if (s == SwizzleSource::k0) { parts[i] = "0.0"; identity = false; }
        else if (s == SwizzleSource::k1) { parts[i] = "1.0"; identity = false; }
        else { parts[i] = base + "." + kComp[uint32_t(s)]; identity &= uint32_t(s) == i; }
    }
    if (identity) return base;
    return "vec4(" + parts[0] + ", " + parts[1] + ", " + parts[2] + ", " + parts[3] + ")";
}

void GlslTranslator::WriteResult(const InstructionResult& r, const std::string& value) {
    const uint32_t mask = r.GetUsedWriteMask();
    if (!mask) return;
    std::string target;
    switch (r.storage_target) {
        case InstructionStorageTarget::kRegister: target = "r[" + RegisterIndex(r.storage_index, r.storage_addressing_mode) + "]"; break;
        case InstructionStorageTarget::kInterpolator: target = "xo[" + std::to_string(r.storage_index & 15) + "]"; break;
        case InstructionStorageTarget::kPosition: target = "xpos"; break;
        case InstructionStorageTarget::kColor: target = "oC[" + std::to_string(r.storage_index & 3) + "]"; break;
        case InstructionStorageTarget::kDepth: target = "gl_FragDepth"; break;
        case InstructionStorageTarget::kPointSizeEdgeFlagKillVertex: return;   // taille de point / drapeau d'arête : ignorés
        case InstructionStorageTarget::kExportAddress:
        case InstructionStorageTarget::kExportData:
            EmitTranslationError("memexport non géré", true);
            return;
        default: return;
    }
    if (r.storage_target == InstructionStorageTarget::kInterpolator && !is_vertex_shader()) return;
    static const char* kComp[] = {"x", "y", "z", "w"};
    std::string v = r.is_clamped ? "clamp(" + value + ", 0.0, 1.0)" : value;
    Line("{ vec4 w = " + v + ";");
    for (uint32_t i = 0; i < 4; i++) {
        if (!(mask >> i & 1)) continue;
        const SwizzleSource s = r.components[i];
        const std::string src = s == SwizzleSource::k0 ? "0.0" : s == SwizzleSource::k1 ? "1.0" : std::string("w.") + kComp[uint32_t(s)];
        if (r.storage_target == InstructionStorageTarget::kDepth) { if (i == 0) Line("  gl_FragDepth = " + src + ";"); }
        else Line("  " + target + "." + kComp[i] + " = " + src + ";");
    }
    Line("}");
}

// ---------- ALU ----------
void GlslTranslator::ProcessAluInstruction(const ParsedAluInstruction& in, uint8_t) {
    if (in.IsNop()) return;
    Line("{");   // portée propre à l'instruction (déclarations temporaires)
    indent_++;
    if (in.is_predicated) { Line("if (" + Condition(false, 0, in.predicate_condition) + ") {"); indent_++; }
    // Les deux moitiés (vecteur, scalaire) lisent l'ancien état : calcul d'abord, écritures ensuite.
    std::string s[3];
    for (uint32_t k = 0; k < in.vector_operand_count; k++) s[k] = Operand(in.vector_operands[k]);
    const bool vecNop = in.IsVectorOpDefaultNop(), scaNop = in.IsScalarOpDefaultNop();
    bool vecWritesP0 = false;
    if (!vecNop) {
        const std::string a = "va", b = "vb", c = "vc";
        Line("{ vec4 va = " + (s[0].empty() ? "vec4(0.0)" : s[0]) + "; vec4 vb = " + (s[1].empty() ? "vec4(0.0)" : s[1]) +
             "; vec4 vc = " + (s[2].empty() ? "vec4(0.0)" : s[2]) + ";");
        std::string e;
        switch (in.vector_opcode) {
            case AluVectorOpcode::kAdd: e = "va + vb"; break;
            case AluVectorOpcode::kMul: e = "xmul(va, vb)"; break;
            case AluVectorOpcode::kMax: e = "mix(vb, va, greaterThanEqual(va, vb))"; break;
            case AluVectorOpcode::kMin: e = "mix(vb, va, lessThan(va, vb))"; break;
            case AluVectorOpcode::kSeq: e = "vec4(equal(va, vb))"; break;
            case AluVectorOpcode::kSgt: e = "vec4(greaterThan(va, vb))"; break;
            case AluVectorOpcode::kSge: e = "vec4(greaterThanEqual(va, vb))"; break;
            case AluVectorOpcode::kSne: e = "vec4(notEqual(va, vb))"; break;
            case AluVectorOpcode::kFrc: e = "va - floor(va)"; break;
            case AluVectorOpcode::kTrunc: e = "trunc(va)"; break;
            case AluVectorOpcode::kFloor: e = "floor(va)"; break;
            case AluVectorOpcode::kMad: e = "xmul(va, vb) + vc"; break;
            case AluVectorOpcode::kCndEq: e = "mix(vc, vb, equal(va, vec4(0.0)))"; break;
            case AluVectorOpcode::kCndGe: e = "mix(vc, vb, greaterThanEqual(va, vec4(0.0)))"; break;
            case AluVectorOpcode::kCndGt: e = "mix(vc, vb, greaterThan(va, vec4(0.0)))"; break;
            case AluVectorOpcode::kDp4: e = "vec4(xdot4(va, vb))"; break;
            case AluVectorOpcode::kDp3: e = "vec4(xdot3(va, vb))"; break;
            case AluVectorOpcode::kDp2Add: e = "vec4(xmul(va.x, vb.x) + xmul(va.y, vb.y) + vc.x)"; break;
            case AluVectorOpcode::kCube:
                // opérande 0 lu en .z_xy : x = composante 2, y = 3, z = 0
                Line("  vec3 q = vec3(va.z, va.w, va.x); vec3 aq = abs(q); float tc, sc, ma, id;");
                Line("  if (aq.z >= aq.x && aq.z >= aq.y) { tc = -q.y; sc = q.z < 0.0 ? -q.x : q.x; ma = 2.0 * q.z; id = q.z < 0.0 ? 5.0 : 4.0; }");
                Line("  else if (aq.y >= aq.x) { tc = q.y < 0.0 ? -q.z : q.z; sc = q.x; ma = 2.0 * q.y; id = q.y < 0.0 ? 3.0 : 2.0; }");
                Line("  else { tc = -q.y; sc = q.x < 0.0 ? q.z : -q.z; ma = 2.0 * q.x; id = q.x < 0.0 ? 1.0 : 0.0; }");
                e = "vec4(tc, sc, ma, id)";
                break;
            case AluVectorOpcode::kMax4:
                e = "vec4((va.x > va.y && va.x > va.z && va.x > va.w) ? va.x : (va.y > va.z && va.y > va.w) ? va.y : (va.z > va.w) ? va.z : va.w)";
                break;
            case AluVectorOpcode::kSetpEqPush: case AluVectorOpcode::kSetpNePush:
            case AluVectorOpcode::kSetpGtPush: case AluVectorOpcode::kSetpGePush: {
                const char* cmp = in.vector_opcode == AluVectorOpcode::kSetpEqPush ? "!= 0.0" :   // condition sur src1
                                  in.vector_opcode == AluVectorOpcode::kSetpNePush ? "!= 0.0" :
                                  in.vector_opcode == AluVectorOpcode::kSetpGtPush ? "> 0.0" : ">= 0.0";
                std::string c1w = std::string("vb.w ") + cmp, c1x = std::string("vb.x ") + cmp;
                if (in.vector_opcode == AluVectorOpcode::kSetpEqPush) { c1w = "vb.w == 0.0"; c1x = "vb.x == 0.0"; }
                Line("  bool np0 = (va.w == 0.0 && " + c1w + ");");
                e = "vec4((va.x == 0.0 && " + c1x + ") ? 0.0 : va.x + 1.0)";
                vecWritesP0 = true;
                break;
            }
            case AluVectorOpcode::kKillEq: case AluVectorOpcode::kKillGt:
            case AluVectorOpcode::kKillGe: case AluVectorOpcode::kKillNe: {
                const char* f = in.vector_opcode == AluVectorOpcode::kKillEq ? "equal" :
                                in.vector_opcode == AluVectorOpcode::kKillGt ? "greaterThan" :
                                in.vector_opcode == AluVectorOpcode::kKillGe ? "greaterThanEqual" : "notEqual";
                Line(std::string("  bool kv = any(") + f + "(va, vb));");
                if (is_pixel_shader()) Line("  if (kv) discard;");
                e = "vec4(kv ? 1.0 : 0.0)";
                break;
            }
            case AluVectorOpcode::kDst: e = "vec4(1.0, xmul(va.y, vb.y), va.z, vb.w)"; break;
            case AluVectorOpcode::kMaxA:
                Line("  int na0 = int(clamp(floor(va.w + 0.5), -256.0, 255.0));");
                e = "mix(vb, va, greaterThanEqual(va, vb))";
                break;
            default:
                EmitTranslationError("opcode vectoriel non géré", true);
                e = "vec4(0.0)";
        }
        Line("  xv = " + e + ";");
        if (vecWritesP0) Line("  xvp0 = np0;");
        if (in.vector_opcode == AluVectorOpcode::kMaxA) Line("  xva0 = na0;");
        Line("}");
        if (vecWritesP0) body_.insert(body_.rfind("{ vec4 va"), "bool xvp0; ");
        if (in.vector_opcode == AluVectorOpcode::kMaxA) body_.insert(body_.rfind("{ vec4 va"), "int xva0; ");
    }
    bool scaWritesP0 = false, scaWritesA0 = false;
    if (!scaNop) {
        std::string a = "0.0", b = "0.0";
        if (in.scalar_operand_count >= 1) {
            const std::string o0 = Operand(in.scalar_operands[0]);
            a = "(" + o0 + ").x";
            b = in.scalar_operand_count >= 2 ? "(" + Operand(in.scalar_operands[1]) + ").x" : "(" + o0 + ").y";
        }
        Line("{ float sa = " + a + "; float sb = " + b + "; float sr;");
        std::string e;
        auto setp = [&](const std::string& cond) {
            Line("  bool np = " + cond + "; sr = np ? 0.0 : 1.0;");
            scaWritesP0 = true;
        };
        switch (in.scalar_opcode) {
            case AluScalarOpcode::kAdds: case AluScalarOpcode::kAddsc0: case AluScalarOpcode::kAddsc1: e = "sa + sb"; break;
            case AluScalarOpcode::kAddsPrev: e = "sa + ps"; break;
            case AluScalarOpcode::kMuls: case AluScalarOpcode::kMulsc0: case AluScalarOpcode::kMulsc1: e = "xmul(sa, sb)"; break;
            case AluScalarOpcode::kMulsPrev: e = "xmul(sa, ps)"; break;
            case AluScalarOpcode::kMulsPrev2:
                e = "(ps == -3.402823466e38 || isinf(ps) || isnan(ps) || isinf(sb) || isnan(sb) || sb <= 0.0) ? -3.402823466e38 : sa * ps";
                break;
            case AluScalarOpcode::kMaxs: e = "sa >= sb ? sa : sb"; break;
            case AluScalarOpcode::kMins: e = "sa < sb ? sa : sb"; break;
            case AluScalarOpcode::kSeqs: e = "sa == 0.0 ? 1.0 : 0.0"; break;
            case AluScalarOpcode::kSgts: e = "sa > 0.0 ? 1.0 : 0.0"; break;
            case AluScalarOpcode::kSges: e = "sa >= 0.0 ? 1.0 : 0.0"; break;
            case AluScalarOpcode::kSnes: e = "sa != 0.0 ? 1.0 : 0.0"; break;
            case AluScalarOpcode::kFrcs: e = "sa - floor(sa)"; break;
            case AluScalarOpcode::kTruncs: e = "trunc(sa)"; break;
            case AluScalarOpcode::kFloors: e = "floor(sa)"; break;
            case AluScalarOpcode::kExp: e = "sa == 0.0 ? 1.0 : exp2(sa)"; break;
            case AluScalarOpcode::kLogc: e = "xclampmax(sa == 1.0 ? 0.0 : log2(sa))"; break;
            case AluScalarOpcode::kLog: e = "sa == 1.0 ? 0.0 : log2(sa)"; break;
            case AluScalarOpcode::kRcpc: e = "xclampmax(sa == 1.0 ? 1.0 : 1.0 / sa)"; break;
            case AluScalarOpcode::kRcpf: e = "xclampzero(sa == 1.0 ? 1.0 : 1.0 / sa)"; break;
            case AluScalarOpcode::kRcp: e = "sa == 1.0 ? 1.0 : 1.0 / sa"; break;
            case AluScalarOpcode::kRsqc: e = "xclampmax(sa == 1.0 ? 1.0 : inversesqrt(sa))"; break;
            case AluScalarOpcode::kRsqf: e = "xclampzero(sa == 1.0 ? 1.0 : inversesqrt(sa))"; break;
            case AluScalarOpcode::kRsq: e = "sa == 1.0 ? 1.0 : inversesqrt(sa)"; break;
            case AluScalarOpcode::kMaxAs:
                Line("  int sa0 = int(clamp(floor(sa + 0.5), -256.0, 255.0));"); scaWritesA0 = true; e = "sa >= sb ? sa : sb"; break;
            case AluScalarOpcode::kMaxAsf:
                Line("  int sa0 = int(clamp(floor(sa), -256.0, 255.0));"); scaWritesA0 = true; e = "sa >= sb ? sa : sb"; break;
            case AluScalarOpcode::kSubs: case AluScalarOpcode::kSubsc0: case AluScalarOpcode::kSubsc1: e = "sa - sb"; break;
            case AluScalarOpcode::kSubsPrev: e = "sa - ps"; break;
            case AluScalarOpcode::kSetpEq: setp("sa == 0.0"); break;
            case AluScalarOpcode::kSetpNe: setp("sa != 0.0"); break;
            case AluScalarOpcode::kSetpGt: setp("sa > 0.0"); break;
            case AluScalarOpcode::kSetpGe: setp("sa >= 0.0"); break;
            case AluScalarOpcode::kSetpInv:
                Line("  bool np = sa == 1.0; sr = np ? 0.0 : (sa == 0.0 ? 1.0 : sa);"); scaWritesP0 = true; break;
            case AluScalarOpcode::kSetpPop:
                Line("  bool np = (sa - 1.0) <= 0.0; sr = np ? 0.0 : sa - 1.0;"); scaWritesP0 = true; break;
            case AluScalarOpcode::kSetpClr:
                Line("  bool np = false; sr = 3.402823466e38;"); scaWritesP0 = true; break;
            case AluScalarOpcode::kSetpRstr:
                Line("  bool np = sa == 0.0; sr = sa;"); scaWritesP0 = true; break;
            case AluScalarOpcode::kKillsEq: case AluScalarOpcode::kKillsGt: case AluScalarOpcode::kKillsGe:
            case AluScalarOpcode::kKillsNe: case AluScalarOpcode::kKillsOne: {
                const char* c = in.scalar_opcode == AluScalarOpcode::kKillsEq ? "sa == 0.0" :
                                in.scalar_opcode == AluScalarOpcode::kKillsGt ? "sa > 0.0" :
                                in.scalar_opcode == AluScalarOpcode::kKillsGe ? "sa >= 0.0" :
                                in.scalar_opcode == AluScalarOpcode::kKillsNe ? "sa != 0.0" : "sa == 1.0";
                Line(std::string("  bool ks = ") + c + ";");
                if (is_pixel_shader()) Line("  if (ks) discard;");
                e = "ks ? 1.0 : 0.0";
                break;
            }
            case AluScalarOpcode::kSqrt: e = "sqrt(sa)"; break;
            case AluScalarOpcode::kSin: e = "sin(sa)"; break;
            case AluScalarOpcode::kCos: e = "cos(sa)"; break;
            case AluScalarOpcode::kRetainPrev: e = "ps"; break;
            default:
                EmitTranslationError("opcode scalaire non géré", true);
                e = "0.0";
        }
        if (!e.empty()) Line("  sr = " + e + ";");
        Line("  xs = sr;");
        if (scaWritesP0) Line("  xsp0 = np;");
        if (scaWritesA0) Line("  xsa0 = sa0;");
        Line("}");
        if (scaWritesP0) body_.insert(body_.rfind("{ float sa"), "bool xsp0; ");
        if (scaWritesA0) body_.insert(body_.rfind("{ float sa"), "int xsa0; ");
    }
    // Écritures (le résultat « vecteur » porte aussi les composantes constantes 0/1)
    if (!vecNop || in.vector_and_constant_result.GetUsedWriteMask())
        WriteResult(in.vector_and_constant_result, vecNop ? "vec4(0.0)" : "xv");
    if (!scaNop) {
        WriteResult(in.scalar_result, "vec4(xs)");
        Line("ps = xs;");
    }
    if (vecWritesP0) Line("p0 = xvp0;");
    if (scaWritesP0) Line("p0 = xsp0;");
    if (in.vector_opcode == AluVectorOpcode::kMaxA && !vecNop) Line("a0 = xva0;");
    if (scaWritesA0) Line("a0 = xsa0;");
    if (in.is_predicated) { indent_--; Line("}"); }
    indent_--;
    Line("}");
}

// ---------- Lectures ----------
void GlslTranslator::ProcessVertexFetchInstruction(const ParsedVertexFetchInstruction& in) {
    using xe::gpu::xenos::VertexFormat;
    if (in.is_predicated) { Line("if (" + Condition(false, 0, in.predicate_condition) + ") {"); indent_++; }
    uint32_t fc = lastVfetchConstant_, stride = lastVfetchStride_;
    std::string index = lastVfetchIndexExpr_;
    if (!in.is_mini_fetch) {
        fc = in.operands[1].storage_index;
        stride = in.attributes.stride;
        index = "(" + Operand(in.operands[0]) + ").x";
        lastVfetchConstant_ = fc; lastVfetchStride_ = stride; lastVfetchIndexExpr_ = index;
    }
    const auto& at = in.attributes;
    // constante de sommets : mot 0 = adresse en mots (bits 2:31), mot 1 = mode d'octets (bits 0:1)
    Line("{ uint f0 = xfetch(" + U(fc * 2) + "), f1 = xfetch(" + U(fc * 2 + 1) + "); uint e = f1 & 3u;");
    Line(std::string("  uint idx = uint(int(") + (at.is_index_rounded ? "floor(" + index + " + 0.5)" : "floor(" + index + ")") + "));");
    Line("  uint dw = (f0 >> 2) + idx * " + U(stride) + " + uint(" + std::to_string(at.offset) + ");");
    const bool sgn = at.is_signed, integer = at.is_integer;
    auto norm8 = [&](const std::string& v, uint32_t bits) {
        const uint32_t max = (1u << bits) - 1, smax = (1u << (bits - 1)) - 1;
        if (sgn) return integer ? "xsigned(" + v + ", " + U(bits) + ")" : "max(xsigned(" + v + ", " + U(bits) + ") / " + F(float(smax)) + ", -1.0)";
        return integer ? "float(" + v + ")" : "float(" + v + ") / " + F(float(max));
    };
    std::string val;
    auto packed = [&](std::initializer_list<std::pair<int, int>> fields) {
        std::string comps[4] = {"0.0", "0.0", "0.0", "1.0"};
        int k = 0;
        for (auto [off, bits] : fields) comps[k++] = norm8("bitfieldExtract(w0, " + std::to_string(off) + ", " + std::to_string(bits) + ")", uint32_t(bits));
        return "vec4(" + comps[0] + ", " + comps[1] + ", " + comps[2] + ", " + comps[3] + ")";
    };
    Line("  uint w0 = xread(dw, e), w1 = xread(dw + 1u, e), w2 = xread(dw + 2u, e), w3 = xread(dw + 3u, e);");
    switch (at.data_format) {
        case VertexFormat::k_32_FLOAT: val = "vec4(uintBitsToFloat(w0), 0.0, 0.0, 1.0)"; break;
        case VertexFormat::k_32_32_FLOAT: val = "vec4(uintBitsToFloat(w0), uintBitsToFloat(w1), 0.0, 1.0)"; break;
        case VertexFormat::k_32_32_32_FLOAT: val = "vec4(uintBitsToFloat(w0), uintBitsToFloat(w1), uintBitsToFloat(w2), 1.0)"; break;
        case VertexFormat::k_32_32_32_32_FLOAT: val = "uintBitsToFloat(uvec4(w0, w1, w2, w3))"; break;
        case VertexFormat::k_16_16_FLOAT: val = "vec4(unpackHalf2x16(w0), 0.0, 1.0)"; break;
        case VertexFormat::k_16_16_16_16_FLOAT: val = "vec4(unpackHalf2x16(w0), unpackHalf2x16(w1))"; break;
        case VertexFormat::k_8_8_8_8: val = packed({{0, 8}, {8, 8}, {16, 8}, {24, 8}}); break;
        case VertexFormat::k_2_10_10_10: val = packed({{0, 10}, {10, 10}, {20, 10}, {30, 2}}); break;
        case VertexFormat::k_10_11_11: val = packed({{0, 11}, {11, 11}, {22, 10}}); break;
        case VertexFormat::k_11_11_10: val = packed({{0, 10}, {10, 11}, {21, 11}}); break;
        case VertexFormat::k_16_16: val = packed({{0, 16}, {16, 16}}); break;
        case VertexFormat::k_16_16_16_16:
            val = "vec4(" + norm8("bitfieldExtract(w0, 0, 16)", 16) + ", " + norm8("bitfieldExtract(w0, 16, 16)", 16) + ", " +
                  norm8("bitfieldExtract(w1, 0, 16)", 16) + ", " + norm8("bitfieldExtract(w1, 16, 16)", 16) + ")";
            break;
        case VertexFormat::k_32: val = "vec4(" + std::string(sgn ? "float(int(w0))" : "float(w0)") + ", 0.0, 0.0, 1.0)"; break;
        case VertexFormat::k_32_32: val = sgn ? "vec4(float(int(w0)), float(int(w1)), 0.0, 1.0)" : "vec4(float(w0), float(w1), 0.0, 1.0)"; break;
        case VertexFormat::k_32_32_32_32: val = sgn ? "vec4(ivec4(w0, w1, w2, w3))" : "vec4(uvec4(w0, w1, w2, w3))"; break;
        default:
            EmitTranslationError("format de sommet non géré", true);
            val = "vec4(0.0)";
    }
    if (at.exp_adjust && at.data_format != VertexFormat::k_32_FLOAT && at.data_format != VertexFormat::k_32_32_FLOAT &&
        at.data_format != VertexFormat::k_32_32_32_FLOAT && at.data_format != VertexFormat::k_32_32_32_32_FLOAT)
        val = "(" + val + ") * " + F(float(1 << (at.exp_adjust > 0 ? at.exp_adjust : 0)) / float(1 << (at.exp_adjust < 0 ? -at.exp_adjust : 0)));
    Line("  xt = " + val + ";");
    Line("}");
    WriteResult(in.result, "xt");
    if (in.is_predicated) { indent_--; Line("}"); }
}

void GlslTranslator::ProcessTextureFetchInstruction(const ParsedTextureFetchInstruction& in) {
    using ucode::FetchOpcode;
    if (in.opcode == FetchOpcode::kSetTextureLod || in.opcode == FetchOpcode::kSetTextureGradientsHorz ||
        in.opcode == FetchOpcode::kSetTextureGradientsVert) return;   // état de LOD : ignoré (approximation)
    if (in.opcode != FetchOpcode::kTextureFetch) { EmitTranslationError("lecture de texture spéciale non gérée", true); return; }
    if (!in.has_result()) return;
    if (in.dimension == xe::gpu::xenos::FetchOpDimension::k3DOrStacked || in.dimension == xe::gpu::xenos::FetchOpDimension::kCube) {
        EmitTranslationError("texture 3D ou cube non gérée", true);
        return;
    }
    usesTextures_ = true;
    if (in.is_predicated) { Line("if (" + Condition(false, 0, in.predicate_condition) + ") {"); indent_++; }
    const uint32_t slot = in.operands[1].storage_index & 31;
    const auto& at = in.attributes;
    std::string coord = "(" + Operand(in.operands[0]) + ").xy";
    if (at.unnormalized_coordinates) coord = "(" + coord + " / vec2(textureSize(tex[" + std::to_string(slot) + "], 0)))";
    if (at.offset_x != 0.0f || at.offset_y != 0.0f)
        coord = "(" + coord + " + vec2(" + F(at.offset_x) + ", " + F(at.offset_y) + ") / vec2(textureSize(tex[" + std::to_string(slot) + "], 0)))";
    if (in.dimension == xe::gpu::xenos::FetchOpDimension::k1D) coord = "vec2((" + Operand(in.operands[0]) + ").x, 0.5)";
    if (is_vertex_shader()) Line("xt = textureLod(tex[" + std::to_string(slot) + "], " + coord + ", 0.0);");
    else if (at.lod_bias != 0.0f) Line("xt = texture(tex[" + std::to_string(slot) + "], " + coord + ", " + F(at.lod_bias) + ");");
    else Line("xt = texture(tex[" + std::to_string(slot) + "], " + coord + ");");
    WriteResult(in.result, "xt");
    if (in.is_predicated) { indent_--; Line("}"); }
}

// ---------- GLSL -> SPIR-V ----------
bool CompileGlsl(int stage, const std::string& source, std::vector<uint32_t>* spirv, std::string* log) {
    // Statique locale (initialisation thread-safe) plutôt que std::call_once : sous MinGW, Clang (TLS natif) ne
    // trouve pas __once_call du libstdc++ de GCC (TLS émulé).
    static const bool initialized = glslang::InitializeProcess();
    (void)initialized;
    const EShLanguage lang = stage == 0 ? EShLangVertex : stage == 1 ? EShLangFragment : stage == 2 ? EShLangCompute : EShLangGeometry;
    glslang::TShader shader(lang);
    const char* src = source.c_str();
    shader.setStrings(&src, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);
    if (!shader.parse(GetDefaultResources(), 450, false, EShMsgDefault)) {
        if (log) *log = shader.getInfoLog();
        return false;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(EShMsgDefault)) {
        if (log) *log = program.getInfoLog();
        return false;
    }
    glslang::GlslangToSpv(*program.getIntermediate(lang), *spirv);
    return true;
}

}  // namespace wk::gpu
