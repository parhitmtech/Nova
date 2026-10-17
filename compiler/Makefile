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

SRCS := src/compiler.cc \
        src/frontend/lexer.cc src/frontend/inputbuf.cc src/frontend/parser.cc \
        src/codegen/bytecode.cc \
        src/runtime/autograd.cc
HDRS := $(wildcard include/*.h)

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
