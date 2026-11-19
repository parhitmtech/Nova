# The dialect's tests, run by lit: ninja check-nova
set(LLVM_TOOLS_DIR ${LLVM_TOOLS_BINARY_DIR})  # FileCheck and the other LLVM test tools
configure_lit_site_cfg(
    ${CMAKE_CURRENT_SOURCE_DIR}/lit.site.cfg.py.in
    ${CMAKE_CURRENT_BINARY_DIR}/lit.site.cfg.py
    MAIN_CONFIG
    ${CMAKE_CURRENT_SOURCE_DIR}/lit.cfg.py
)
add_lit_testsuite(check-nova "Running the nova dialect's tests"
    ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS nova-opt nova-lower
)
