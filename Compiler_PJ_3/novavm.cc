#define NOVA_NOVM_BUILD  // suppress compiler.cc's main()
#include "bytecode.h"
#include "compiler.h"
#include <cstdio>
#include <cstring>
#include <string>

// execute_program is defined in compiler.cc (not declared in compiler.h)
void execute_program(struct InstructionNode* head);

int main(int argc, char* argv[])
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    if (argc < 2) {
        fprintf(stderr, "Usage: novavm <program.nbc>\n");
        return 1;
    }

    const char* path = argv[1];
    size_t plen = strlen(path);
    if (plen < 5 || strcmp(path + plen - 4, ".nbc") != 0) {
        fprintf(stderr, "Error: file must have .nbc extension\n");
        return 1;
    }

    struct InstructionNode* program = load_bytecode(path);
    if (!program) {
        fprintf(stderr, "novavm: failed to load %s\n", path);
        return 1;
    }

    execute_program(program);
    fflush(stdout);
    return 0;
}
