cmake_policy(SET CMP0169 OLD)

option(SPARSITY_USE_SYSTEM_BOOST "Use system Boost headers instead of FetchContent" OFF)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

add_library(boost_headers INTERFACE)
target_link_libraries(boost_headers INTERFACE Threads::Threads)

if(SPARSITY_USE_SYSTEM_BOOST)
    find_package(Boost 1.92 REQUIRED)
    target_include_directories(boost_headers INTERFACE ${Boost_INCLUDE_DIRS})
else()
    include(FetchContent)
    FetchContent_Declare(
            Boost
            URL https://archives.boost.io/release/1.92.0/source/boost_1_92_0.tar.gz
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_GetProperties(Boost)
    if(NOT boost_POPULATED)
        FetchContent_Populate(Boost)
    endif()
    target_include_directories(boost_headers INTERFACE ${boost_SOURCE_DIR})
endif()

add_library(boost_wrapper)

target_sources(
        boost_wrapper
        PUBLIC
        FILE_SET CXX_MODULES
        FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/boost_wrapper.cppm
)

target_link_libraries(
        boost_wrapper
        PUBLIC
        boost_headers
        OpenSSL::SSL
        OpenSSL::Crypto
)
