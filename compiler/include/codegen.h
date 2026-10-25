#ifndef NOVA_CODEGEN_H
#define NOVA_CODEGEN_H
// Code generators: CUDA source. (Native code will come from LLVM: see docs/ROADMAP.md.)

#include <string>
#include "compiler.h"

// src/codegen/cuda.cc  (--emit-cuda)
void generate_cuda(struct InstructionNode* program, const std::string& outputFile);

#endif
