add_library(http)

target_sources(
        http
        PUBLIC
        FILE_SET CXX_MODULES
        FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/http.cppm
)

target_link_libraries(
        http
        PUBLIC
        boost_wrapper
)
