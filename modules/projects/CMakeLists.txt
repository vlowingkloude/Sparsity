add_library(projects)

target_sources(
        projects
        PUBLIC
        FILE_SET CXX_MODULES
        FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/projects.cppm
)

target_link_libraries(
        projects
        PUBLIC
        files
        json
)
