add_library(uploads)

target_sources(
        uploads
        PUBLIC
        FILE_SET CXX_MODULES
        FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/uploads.cppm
)

target_link_libraries(
        uploads
        PUBLIC
        json
        images
)
