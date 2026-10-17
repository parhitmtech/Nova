#ifndef NOVA_CODEGEN_H
#define NOVA_CODEGEN_H
// Code generators: CUDA source, x86-64 assembly, and the in-process x86 JIT.

#include <string>
#include <unordered_map>
#include <unordered_set>
#include "compiler.h"

struct ColoringResult;

// src/codegen/cuda.cc  (--emit-cuda)
void generate_cuda(struct InstructionNode* program, const std::string& outputFile);

// src/codegen/x86_asm.cc  (--emit-asm / --build): NASM, Linux System V ABI
void generate_x86(struct InstructionNode* program, const std::string& outputFile,
                  const ColoringResult* coloring = nullptr);

// src/codegen/jit.cc — native code for simple single-parameter int functions (Windows only at
// runtime; -DNOVA_NO_JIT disables it). Returns nullptr when a function cannot be compiled.
void* jit_compile_function(struct InstructionNode* head);

// JIT state shared with the interpreter's CALL handler (defined in compiler.cc).
extern std::unordered_map<InstructionNode*, int> jit_call_counts;
extern std::unordered_map<InstructionNode*, void*> jit_compiled;
extern std::unordered_set<InstructionNode*> jit_deoptimized;

#endif
