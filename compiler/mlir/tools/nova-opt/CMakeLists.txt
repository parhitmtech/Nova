# nova-opt: reads MLIR, runs the passes named on its command line, prints the result (as mlir-opt does)
add_llvm_executable(nova-opt nova-opt.cpp)
llvm_update_compile_flags(nova-opt)
target_link_libraries(nova-opt PRIVATE
    MLIRNova
    MLIRArithDialect
    MLIRFuncDialect
    MLIRSCFDialect
    MLIROptLib
    MLIRTransforms
)
mlir_check_all_link_libraries(nova-opt)
