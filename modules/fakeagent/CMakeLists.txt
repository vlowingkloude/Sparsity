add_library(fakeagent)

target_sources(
        fakeagent
        PUBLIC
        FILE_SET CXX_MODULES
        FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/fakeagent.cppm
)

target_link_libraries(
        fakeagent
        PUBLIC
        json
        boost_wrapper
)
