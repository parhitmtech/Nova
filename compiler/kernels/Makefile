## The matmul kernels' benchmark (needs nvcc and cuBLAS: build it on a GPU machine such as SOL)
##   make                 build ./bench for ARCH (sm_80: A100; sm_90: H100)
##   make run             build and run the default cases
##   make cpu-check       check the kernels' results on the CPU (g++ only, no GPU)

NVCC  ?= nvcc
ARCH  ?= sm_80
FLAGS := -O3 -std=c++17 -arch=$(ARCH) -lineinfo

.PHONY: all run cpu-check clean

all: bench

bench: bench.cu matmul_kernels.cuh
	$(NVCC) $(FLAGS) bench.cu -o bench -lcublas

run: bench
	./bench

cpu-check: cpu_check.cc matmul_kernels.cuh
	$(CXX) -std=c++20 -O1 -pthread -x c++ cpu_check.cc -o cpu_check
	./cpu_check

clean:
	rm -f bench cpu_check cpu_check.exe
