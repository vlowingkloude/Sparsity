add_library(agent)

target_sources(
        agent
        PUBLIC
        FILE_SET CXX_MODULES
        FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/agent.cppm
)

target_link_libraries(
        agent
        PUBLIC
        json
        llm
        session
        compaction
        tools
        models
        pricing
        config
        files
        boost_wrapper
)
