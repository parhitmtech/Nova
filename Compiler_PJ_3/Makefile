## Nova compiler build/test Makefile
## See README.md for the libssh2 dependency (apt-get install libssh2-1-dev).

CXX		 := g++
CXXFLAGS := -O2 -std=c++17
# libssh2 is optional: compiler.cc only enables SOL/SSH support when <libssh2.h> is found.
LDLIBS	 := $(shell pkg-config --libs libssh2 2>/dev/null)

SRCS	 := compiler.cc lexer.cc inputbuf.cc parser2.cc bytecode.cc autograd.cc
BIN		 := compiler

export NOVA_HOME := $(CURDIR)

.PHONY: all build test clean

all: build 

build: $(BIN)

$(BIN): $(SRCS) *.h
	$(CXX) $(CXXFLAGS) -o $(BIN) $(SRCS) $(LDLIBS)

test: build	
	node run_tests.js $(FILTER)

clean:
	rm -rf $(BIN) $(BIN).exe *.o *.asm *.nbc
	rm -rf _tmp

