## Fuzzing the front end with libFuzzer (clang only):
##   make            build fuzz_frontend
##   make regress    run the inputs that once failed (regressions/), each once
##   make run        fuzz for $(SECONDS) seconds (default 60), starting from the golden tests and the standard library
## A crash leaves its input in crash-*: reproduce it with ./fuzz_frontend crash-<hash>; once fixed, keep it in
## regressions/ under a name that says what it found.

CXX      ?= clang++
CXXFLAGS ?= -O1 -g
SECONDS  ?= 60
SANITIZE := -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined
FRONTEND := ../src/frontend/source.cc ../src/frontend/diagnostics.cc ../src/frontend/interner.cc \
            ../src/frontend/lexer.cc ../src/frontend/parse.cc ../src/frontend/parse_tree.cc \
            ../src/frontend/check.cc ../src/frontend/check_types.cc ../src/frontend/builtins.cc

.PHONY: all regress run clean
all: fuzz_frontend

fuzz_frontend: fuzz_frontend.cc $(FRONTEND) $(wildcard ../include/*.h ../include/*.def)
	$(CXX) -std=c++17 -I../include $(CXXFLAGS) $(SANITIZE) -o $@ fuzz_frontend.cc $(FRONTEND)

regress: fuzz_frontend
	./fuzz_frontend regressions/*

run: fuzz_frontend
	mkdir -p corpus
	cp regressions/* corpus/
	cp ../tests/*.nova ../stdlib/*.nova corpus/ 2>/dev/null; true
	find ../stdlib -name '*.nova' -exec cp {} corpus/ \;
	./fuzz_frontend corpus -dict=nova.dict -max_total_time=$(SECONDS) -timeout=10 -rss_limit_mb=2048 \
	    -max_len=4096 -print_final_stats=1

clean:
	rm -rf fuzz_frontend corpus crash-* leak-* timeout-* oom-*
