# The dialect as a library, MLIRNova: what nova-opt, and later Nova's compiler, link against
add_mlir_dialect_library(MLIRNova
    NovaDialect.cpp
    NovaOps.cpp

    ADDITIONAL_HEADER_DIRS
    ${PROJECT_SOURCE_DIR}/include/Nova

    DEPENDS
    MLIRNovaOpsIncGen  # the generated headers exist before these files compile

    LINK_LIBS PUBLIC
    MLIRIR
    MLIRSideEffectInterfaces
)
