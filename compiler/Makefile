## Nova compiler build/test Makefile
##   make                 build build/nova
##   make novavm          build the standalone bytecode VM (build/novavm)
##   make test            build, then run the golden tests in tests/ (FILTER=name to narrow)
##   make CXX=clang++ CXXFLAGS="-O1 -g -fsanitize=address,undefined"   any compiler/flags
##   make -B CXXFLAGS="-O2 -DNOVA_NO_JIT"   Windows: always interpret (the x86 JIT is Windows-only)
## libssh2 is optional: SOL/SSH support is enabled only when pkg-config finds it
## (apt-get install libssh2-1-dev).

CXXFLAGS ?= -O2
CPPFLAGS += -std=c++17 -Iinclude $(shell pkg-config --cflags libssh2 2>/dev/null)
LDLIBS   += $(shell pkg-config --libs libssh2 2>/dev/null)

ifeq ($(OS),Windows_NT)
EXE := .exe
endif

SRCS := src/driver/main.cc src/driver/repl.cc src/driver/debugger.cc \
        src/frontend/lexer.cc src/frontend/inputbuf.cc src/frontend/imports.cc \
        src/frontend/parser.cc src/frontend/parser_state.cc src/frontend/parser_util.cc \
        src/frontend/parser_classes.cc src/frontend/parser_builtins.cc \
        src/frontend/parser_statements.cc src/frontend/parser_expressions.cc \
        src/runtime/state.cc src/runtime/memory.cc src/runtime/tensor.cc src/runtime/interpreter.cc \
        src/runtime/interp_core.cc src/runtime/interp_calls.cc src/runtime/interp_tensor.cc \
        src/runtime/interp_integrations.cc \
        src/runtime/memoize.cc \
        src/codegen/bytecode.cc src/codegen/cuda.cc src/codegen/x86_asm.cc src/codegen/jit.cc \
        src/opt/constant_fold.cc src/opt/copy_propagation.cc src/opt/dead_code.cc \
        src/opt/licm.cc src/opt/inline.cc src/opt/tensor_release.cc \
        src/regalloc/liveness.cc src/regalloc/coloring.cc \
        src/ir/dump.cc \
        src/integrations/sol.cc src/integrations/hf.cc \
        src/runtime/autograd.cc
HDRS := $(wildcard include/*.h src/*/*.h)

BUILD  := build
BIN    := $(BUILD)/nova$(EXE)
VM_BIN := $(BUILD)/novavm$(EXE)

export NOVA_HOME := $(CURDIR)

.PHONY: all build novavm test clean

all: build

build: $(BIN)

$(BIN): $(SRCS) $(HDRS)
	@mkdir -p $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $(SRCS) $(LDLIBS)

novavm: $(VM_BIN)

# The VM reuses the interpreter from compiler.cc with its main() compiled out.
$(VM_BIN): $(SRCS) src/driver/novavm_main.cc $(HDRS)
	@mkdir -p $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -DNOVA_NOVM_BUILD -o $@ $(SRCS) src/driver/novavm_main.cc $(LDLIBS)

test: build
	NOVA_COMPILER_PATH=$(CURDIR)/$(BIN) node tests/run.js $(FILTER)

clean:
	rm -rf $(BUILD) _test_*
