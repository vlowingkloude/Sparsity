cmake_policy(SET CMP0169 OLD)

include(FetchContent)

FetchContent_Declare(
        stb
        URL https://github.com/nothings/stb/archive/refs/heads/master.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_GetProperties(stb)
if(NOT stb_POPULATED)
    FetchContent_Populate(stb)
endif()

add_library(images)

target_sources(
        images
        PUBLIC
        FILE_SET CXX_MODULES
        FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/images.cppm
)

target_include_directories(images PUBLIC ${stb_SOURCE_DIR} ${stb_SOURCE_DIR}/deprecated)
