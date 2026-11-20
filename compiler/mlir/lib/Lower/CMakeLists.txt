# Lowering the checked tree to MLIR (LowerTree.cpp)
add_mlir_library(NovaLower
    LowerTree.cpp

    ADDITIONAL_HEADER_DIRS
    ${PROJECT_SOURCE_DIR}/include/Nova

    DEPENDS
    MLIRNovaOpsIncGen

    LINK_LIBS PUBLIC
    MLIRNova
    MLIRArithDialect
    MLIRFuncDialect
    MLIRSCFDialect
    MLIRIR
    NovaFrontend
)
