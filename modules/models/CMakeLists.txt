add_library(models)

target_sources(
        models
        PUBLIC
        FILE_SET CXX_MODULES
        FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/models.cppm
)

target_link_libraries(
        models
        PUBLIC
        json
        http
)
