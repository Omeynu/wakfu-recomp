// Traducteur microcode Xenos -> GLSL 4.50 (Vulkan), sur l'analyseur de Xenia (ShaderTranslator).
//
// Conventions d'interface (doivent correspondre au pipeline du runtime) :
//   set 0, binding 0 : uniform Consts  { vec4 c[512]; uvec4 b[2]; uvec4 l[8]; }  (c : VS 0-255, PS 256-511 ;
//                      b : 256 booléens ; l : 32 constantes de boucle)
//   set 0, binding 1 : buffer Mem[4]   { uint mem[]; }  mémoire physique invitée, 4 tranches de 128 Mo
//   set 0, binding 2 : uniform Fetch   { uvec4 f[48]; } constantes de lecture (texture : 6 mots, sommets : 2 mots)
//   set 0, binding 3 : sampler2D tex[32]
//   push constants   : vec4 ndcScale, ndcOffset (sommets) ; int vertexIndexOffset
//   VS : sorties location n = interpolateur n ; PS : entrées location n, sorties location n = oCn.
#pragma once
#include <string>
#include <vector>
#include "xenia/gpu/shader_translator.h"

namespace wk::gpu {

class GlslTranslator : public xe::gpu::ShaderTranslator {
public:
    // Analyse déjà faite (Shader::AnalyzeUcode). Retourne le GLSL, ou "" et `error` si non traduisible.
    std::string Translate(xe::gpu::Shader& shader, std::string* error);

protected:
    void Reset() override;
    void EmitTranslationError(const char* message, bool is_fatal) override;
    void StartTranslation() override;
    std::vector<uint8_t> CompleteTranslation() override;

    void ProcessLabel(uint32_t cf_index) override;
    void ProcessExecInstructionBegin(const xe::gpu::ParsedExecInstruction& instr) override;
    void ProcessExecInstructionEnd(const xe::gpu::ParsedExecInstruction& instr) override;
    void ProcessLoopStartInstruction(const xe::gpu::ParsedLoopStartInstruction& instr) override;
    void ProcessLoopEndInstruction(const xe::gpu::ParsedLoopEndInstruction& instr) override;
    void ProcessCallInstruction(const xe::gpu::ParsedCallInstruction& instr) override;
    void ProcessReturnInstruction(const xe::gpu::ParsedReturnInstruction& instr) override;
    void ProcessJumpInstruction(const xe::gpu::ParsedJumpInstruction& instr) override;
    void ProcessVertexFetchInstruction(const xe::gpu::ParsedVertexFetchInstruction& instr) override;
    void ProcessTextureFetchInstruction(const xe::gpu::ParsedTextureFetchInstruction& instr) override;
    void ProcessAluInstruction(const xe::gpu::ParsedAluInstruction& instr, uint8_t memexport_eM_potentially_written_before) override;

private:
    std::string Operand(const xe::gpu::InstructionOperand& op) const;
    std::string RegisterIndex(uint32_t index, xe::gpu::InstructionStorageAddressingMode mode) const;
    void WriteResult(const xe::gpu::InstructionResult& r, const std::string& vec4Value);
    std::string Condition(bool isBool, uint32_t boolIndex, bool condition) const;
    void Line(const std::string& s);

    std::string body_;
    std::vector<std::string> errors_;
    int indent_ = 0;
    bool usesTextures_ = false;
    uint32_t lastVfetchConstant_ = 0, lastVfetchStride_ = 0;
    int lastVfetchIndexReg_ = 0;
    std::string lastVfetchIndexExpr_;
};

// Compile un GLSL en SPIR-V (glslang). stage : 0 sommets, 1 pixels.
// stage : 0 sommets, 1 pixels, 2 calcul, 3 géométrie
bool CompileGlsl(int stage, const std::string& source, std::vector<uint32_t>* spirv, std::string* log);

}  // namespace wk::gpu
