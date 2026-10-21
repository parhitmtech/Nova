#ifndef NOVA_INTERP_INTERNAL_H
#define NOVA_INTERP_INTERNAL_H
// Private to src/runtime: one handler per IR instruction type. Each executes the instruction at
// pc and returns the next instruction (execute_program's dispatch loop lives in interpreter.cc).

#include "compiler.h"

InstructionNode* exec_in(InstructionNode* pc);
InstructionNode* exec_out(InstructionNode* pc);
InstructionNode* exec_assign(InstructionNode* pc);
InstructionNode* exec_cjmp(InstructionNode* pc);
InstructionNode* exec_jmp(InstructionNode* pc);
InstructionNode* exec_array_read(InstructionNode* pc);
InstructionNode* exec_array_write(InstructionNode* pc);
InstructionNode* exec_alloc(InstructionNode* pc);
InstructionNode* exec_strcat(InstructionNode* pc);
InstructionNode* exec_scmp(InstructionNode* pc);
InstructionNode* exec_assign_f(InstructionNode* pc);
InstructionNode* exec_assign_d(InstructionNode* pc);
InstructionNode* exec_cast(InstructionNode* pc);
InstructionNode* exec_call(InstructionNode* pc);
InstructionNode* exec_ret(InstructionNode* pc);
InstructionNode* exec_tensor_call(InstructionNode* pc);
InstructionNode* exec_container_call(InstructionNode* pc);  // the CON_* ops (interp_containers.cc)
InstructionNode* exec_tensor_release(InstructionNode* pc);
InstructionNode* exec_hf_infer(InstructionNode* pc);
InstructionNode* exec_sol_call(InstructionNode* pc);

#endif
