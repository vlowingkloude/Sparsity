add_library(tools)

target_sources(
        tools
        PUBLIC
        FILE_SET CXX_MODULES
        FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/tools.cppm
)

target_link_libraries(
        tools
        PUBLIC
        json
        llm
        config
        http
        images
)
