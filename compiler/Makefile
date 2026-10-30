## Nova compiler build/test Makefile
##   make                 build build/nova
##   make test            build, then run the golden tests in tests/ (FILTER=name to narrow)
##   make CXX=clang++ CXXFLAGS="-O1 -g -fsanitize=address,undefined"   any compiler/flags
## libssh2 is optional: SOL/SSH support is enabled only when pkg-config finds it
## (apt-get install libssh2-1-dev).

CXXFLAGS ?= -O2
CPPFLAGS += -std=c++17 -Iinclude $(shell pkg-config --cflags libssh2 2>/dev/null)
LDLIBS   += $(shell pkg-config --libs libssh2 2>/dev/null)

ifeq ($(OS),Windows_NT)
EXE := .exe
endif

SRCS := src/driver/main.cc src/driver/repl.cc src/driver/debugger.cc \
        src/frontend/source.cc src/frontend/interner.cc src/frontend/lexer.cc src/frontend/imports.cc \
        src/frontend/parse.cc src/frontend/parse_tree.cc src/frontend/check.cc \
        src/frontend/parser.cc src/frontend/parser_state.cc src/frontend/parser_util.cc \
        src/frontend/parser_classes.cc src/frontend/parser_builtins.cc \
        src/frontend/parser_statements.cc src/frontend/parser_expressions.cc  src/frontend/parser_containers.cc src/frontend/parser_objects.cc \
        src/runtime/state.cc src/runtime/memory.cc src/runtime/tensor.cc src/runtime/interpreter.cc \
        src/runtime/interp_core.cc src/runtime/interp_calls.cc src/runtime/interp_tensor.cc \
        src/runtime/interp_integrations.cc src/runtime/interp_containers.cc src/runtime/interp_objects.cc \
        src/runtime/memoize.cc \
        src/codegen/cuda.cc \
        src/opt/constant_fold.cc src/opt/copy_propagation.cc src/opt/dead_code.cc \
        src/opt/licm.cc src/opt/inline.cc src/opt/tensor_release.cc \
        src/regalloc/liveness.cc \
        src/ir/dump.cc \
        src/integrations/sol.cc src/integrations/hf.cc \
        src/runtime/autograd.cc
HDRS := $(wildcard include/*.h src/*/*.h)

BUILD  := build
BIN    := $(BUILD)/nova$(EXE)

export NOVA_HOME := $(CURDIR)

.PHONY: all build test clean

all: build

build: $(BIN)

$(BIN): $(SRCS) $(HDRS)
	@mkdir -p $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $(SRCS) $(LDLIBS)

test: build
	NOVA_COMPILER_PATH=$(CURDIR)/$(BIN) node tests/run.js $(FILTER)

clean:
	rm -rf $(BUILD) _test_*
