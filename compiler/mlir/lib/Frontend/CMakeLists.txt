# The Nova front end (scanner, parser, check pass), built from compiler/src/frontend: the same sources the
# interpreter's Makefile compiles, so both lowerings read the same checked tree
set(NOVA_FRONTEND ${PROJECT_SOURCE_DIR}/../src/frontend)
add_library(NovaFrontend STATIC
    ${NOVA_FRONTEND}/source.cc
    ${NOVA_FRONTEND}/interner.cc
    ${NOVA_FRONTEND}/lexer.cc
    ${NOVA_FRONTEND}/parse.cc
    ${NOVA_FRONTEND}/parse_tree.cc
    ${NOVA_FRONTEND}/check.cc
    ${NOVA_FRONTEND}/check_types.cc
    ${NOVA_FRONTEND}/builtins.cc
)
target_include_directories(NovaFrontend PUBLIC ${PROJECT_SOURCE_DIR}/../include)
