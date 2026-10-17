#ifndef NOVA_CODEGEN_H
#define NOVA_CODEGEN_H
// Code generators that write a program out in another language.

#include <string>
#include "compiler.h"

// src/codegen/cuda.cc  (--emit-cuda)
void generate_cuda(struct InstructionNode* program, const std::string& outputFile);

#endif
