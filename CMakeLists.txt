cmake_minimum_required(VERSION 4.2)

project(Sparsity LANGUAGES CXX)

option(SPARSITY_TEST "Build Sparsity tests instead of the application" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPERIMENTAL_CXX_MODULE_CMAKE_API ON)
set(SPARSITY_FRONTEND_DIST_DIRECTORY "${CMAKE_BINARY_DIR}/frontend/dist")
set(SPARSITY_FRONTEND_WORK_DIRECTORY "${CMAKE_BINARY_DIR}/frontend/work")

add_subdirectory(modules/json)
add_subdirectory(modules/boost)
add_subdirectory(modules/config)
add_subdirectory(modules/images)
add_subdirectory(modules/http)
add_subdirectory(modules/llm)
add_subdirectory(modules/pricing)

add_subdirectory(modules/files)
add_subdirectory(modules/watcher)
add_subdirectory(modules/terminal)
add_subdirectory(modules/lsp)
add_subdirectory(modules/projects)
add_subdirectory(modules/models)
add_subdirectory(modules/session)
add_subdirectory(modules/tools)
add_subdirectory(modules/uploads)
add_subdirectory(modules/compaction)
add_subdirectory(modules/agent)
add_subdirectory(modules/fakeagent)
add_subdirectory(modules/server)

if(SPARSITY_TEST)
    enable_testing()
    add_subdirectory(tests)
else()
    add_subdirectory(main)

    find_program(COREPACK_EXECUTABLE corepack)
    if(COREPACK_EXECUTABLE)
        set(SPARSITY_NPM_COMMAND "${COREPACK_EXECUTABLE}" npm)
    else()
        find_program(NPM_EXECUTABLE npm REQUIRED)
        set(SPARSITY_NPM_COMMAND "${NPM_EXECUTABLE}")
    endif()

    file(GLOB_RECURSE SPARSITY_FRONTEND_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/frontend/src/*")

    set(
            SPARSITY_FRONTEND_FILES
            "${CMAKE_CURRENT_SOURCE_DIR}/frontend/index.html"
            "${CMAKE_CURRENT_SOURCE_DIR}/frontend/package.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/frontend/package-lock.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/frontend/tsconfig.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/frontend/vite.config.ts"
    )

    add_custom_command(
            OUTPUT "${SPARSITY_FRONTEND_WORK_DIRECTORY}/node_modules/.sparsity-installed"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${SPARSITY_FRONTEND_WORK_DIRECTORY}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${SPARSITY_FRONTEND_FILES}
                    "${SPARSITY_FRONTEND_WORK_DIRECTORY}"
            COMMAND ${CMAKE_COMMAND} -E env "npm_config_cache=${CMAKE_BINARY_DIR}/npm-cache" ${SPARSITY_NPM_COMMAND} ci --no-audit --no-fund
            COMMAND ${CMAKE_COMMAND} -E touch "${SPARSITY_FRONTEND_WORK_DIRECTORY}/node_modules/.sparsity-installed"
            WORKING_DIRECTORY "${SPARSITY_FRONTEND_WORK_DIRECTORY}"
            DEPENDS
            frontend/package.json
            frontend/package-lock.json
            COMMENT "Installing the Sparsity frontend dependencies"
            VERBATIM
    )

    add_custom_command(
            OUTPUT "${SPARSITY_FRONTEND_DIST_DIRECTORY}/index.html"
            COMMAND ${CMAKE_COMMAND} -E rm -rf "${SPARSITY_FRONTEND_WORK_DIRECTORY}/src"
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${CMAKE_CURRENT_SOURCE_DIR}/frontend/src"
                    "${SPARSITY_FRONTEND_WORK_DIRECTORY}/src"
            COMMAND ${CMAKE_COMMAND} -E env "npm_config_cache=${CMAKE_BINARY_DIR}/npm-cache" ${SPARSITY_NPM_COMMAND} run build -- --outDir "${SPARSITY_FRONTEND_DIST_DIRECTORY}" --emptyOutDir
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${SPARSITY_FRONTEND_WORK_DIRECTORY}/node_modules/monaco-editor/min/vs"
                    "${SPARSITY_FRONTEND_DIST_DIRECTORY}/monaco/vs"
            WORKING_DIRECTORY "${SPARSITY_FRONTEND_WORK_DIRECTORY}"
            DEPENDS
            ${SPARSITY_FRONTEND_FILES}
            ${SPARSITY_FRONTEND_SOURCES}
            "${SPARSITY_FRONTEND_WORK_DIRECTORY}/node_modules/.sparsity-installed"
            COMMENT "Building the Sparsity frontend"
            VERBATIM
    )

    add_custom_target(
            sparsity_frontend
            ALL
            DEPENDS "${SPARSITY_FRONTEND_DIST_DIRECTORY}/index.html"
    )

    add_dependencies(sparsity sparsity_frontend)
endif()
