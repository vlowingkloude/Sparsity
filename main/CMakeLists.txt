add_executable(sparsity main.cpp)

target_compile_definitions(
        sparsity
        PRIVATE
        SPARSITY_DEFAULT_STATIC_DIRECTORY="${SPARSITY_FRONTEND_DIST_DIRECTORY}"
)

target_link_libraries(
        sparsity
        PRIVATE
        files
        server
)
