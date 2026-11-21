include(FetchContent)

FetchContent_Declare(
        tomlplusplus
        URL https://github.com/marzer/tomlplusplus/archive/refs/tags/v3.4.0.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(tomlplusplus)

add_library(config)

target_sources(
        config
        PUBLIC
        FILE_SET CXX_MODULES
        FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/config.cppm
)

target_link_libraries(
        config
        PUBLIC
        tomlplusplus::tomlplusplus
)
