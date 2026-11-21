add_library(pricing)

target_sources(
        pricing
        PUBLIC
        FILE_SET CXX_MODULES
        FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/pricing.cppm
)

target_link_libraries(
        pricing
        PUBLIC
        llm
)
