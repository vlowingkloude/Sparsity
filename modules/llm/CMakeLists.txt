add_library(llm)

target_sources(
        llm
        PUBLIC
        FILE_SET CXX_MODULES
        FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/llm.cppm
)

target_link_libraries(
        llm
        PUBLIC
        json
        http
)
