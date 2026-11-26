# Nova's C runtime (nova_runtime.c), a static library a compiled Nova program links with
add_library(nova_runtime STATIC nova_runtime.c)
set_target_properties(nova_runtime PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
