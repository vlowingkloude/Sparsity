add_executable(sparsity_tests main.cpp)

target_link_libraries(
        sparsity_tests
        PRIVATE
        files
        lsp
)

add_test(NAME sparsity_tests COMMAND sparsity_tests)
set_tests_properties(sparsity_tests PROPERTIES TIMEOUT 30)

add_executable(tools_smoke tools_smoke.cpp)
target_link_libraries(tools_smoke PRIVATE tools llm config json)
add_test(NAME tools_smoke COMMAND tools_smoke)

add_executable(images_smoke images_smoke.cpp)
target_link_libraries(images_smoke PRIVATE images)
add_test(NAME images_smoke COMMAND images_smoke)
