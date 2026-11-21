add_library(compaction)

target_sources(
        compaction
        PUBLIC
        FILE_SET CXX_MODULES
        FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/compaction.cppm
)

target_link_libraries(
        compaction
        PUBLIC
        json
        llm
        session
        config
        http
)
