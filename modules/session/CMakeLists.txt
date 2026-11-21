add_library(session)

target_sources(
        session
        PUBLIC
        FILE_SET CXX_MODULES
        FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/session.cppm
)

target_link_libraries(
        session
        PUBLIC
        json
        llm
        pricing
)
