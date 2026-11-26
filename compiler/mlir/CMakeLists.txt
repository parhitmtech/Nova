# Nova's MLIR dialect (roadmap step C, docs/DESIGN.md). An out-of-tree MLIR project: it builds against an
# installed LLVM / MLIR (one pinned release, 20) instead of inside LLVM's source tree.
#   cmake -G Ninja -S compiler/mlir -B ~/nova-build -DMLIR_DIR=/usr/lib/llvm-20/lib/cmake/mlir \
#         -DLLVM_EXTERNAL_LIT=$HOME/venvs/lit/bin/lit
#   ninja -C ~/nova-build check-nova
cmake_minimum_required(VERSION 3.20)
project(nova-mlir LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17 CACHE STRING "C++ standard to conform to")

find_package(MLIR REQUIRED CONFIG)
message(STATUS "Using MLIRConfig.cmake in: ${MLIR_DIR}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

# MLIR's CMake helpers: TableGen rules, add_mlir_dialect, add_mlir_dialect_library, lit test suites
set(LLVM_RUNTIME_OUTPUT_INTDIR ${CMAKE_BINARY_DIR}/bin)
set(LLVM_LIBRARY_OUTPUT_INTDIR ${CMAKE_BINARY_DIR}/lib)
set(MLIR_BINARY_DIR ${CMAKE_BINARY_DIR})
list(APPEND CMAKE_MODULE_PATH "${MLIR_CMAKE_DIR}")
list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(TableGen)
include(AddLLVM)
include(AddMLIR)
include(HandleLLVMOptions)

include_directories(${LLVM_INCLUDE_DIRS})
include_directories(${MLIR_INCLUDE_DIRS})
include_directories(${PROJECT_SOURCE_DIR}/include)
include_directories(${PROJECT_BINARY_DIR}/include)  # the headers TableGen generates
link_directories(${LLVM_BUILD_LIBRARY_DIR})
add_definitions(${LLVM_DEFINITIONS})

add_subdirectory(include)
add_subdirectory(lib)
add_subdirectory(runtime)
add_subdirectory(tools)
add_subdirectory(test)
