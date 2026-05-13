#pragma once
#include <cstdint>
#include <string>
#include "compiler.h"

static const char   NBC_MAGIC[4] = {'N','O','V','A'};
static const uint16_t NBC_VERSION = 0x0002;

// Emit the current IR + memory state to a .nbc binary file.
// program   — head of the main instruction list
// outPath   — destination file path (should end in .nbc)
void emit_bytecode(struct InstructionNode* program, const std::string& outPath);

// Load a .nbc file, restore mem/fmem/dmem/strMem, rebuild functionTable,
// and return the head of the main instruction list ready for execute_program().
// Returns nullptr on error.
struct InstructionNode* load_bytecode(const std::string& path);
