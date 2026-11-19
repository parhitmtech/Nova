# nova-lower: reads a Nova program, checks it, and prints it as MLIR
add_llvm_executable(nova-lower nova-lower.cpp)
llvm_update_compile_flags(nova-lower)
target_link_libraries(nova-lower PRIVATE NovaLower)
mlir_check_all_link_libraries(nova-lower)
