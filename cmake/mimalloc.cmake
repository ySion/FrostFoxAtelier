include(FetchContent)

file(READ "${CMAKE_CURRENT_LIST_DIR}/dependencies-lock.json" _dependencies)
string(JSON _mi_url GET "${_dependencies}" mimalloc url)
string(JSON _mi_sha256 GET "${_dependencies}" mimalloc sha256)

# One shared allocator per process; use the official Windows CRT redirector.
set(MI_BUILD_SHARED ON CACHE BOOL "" FORCE)
set(MI_BUILD_STATIC OFF CACHE BOOL "" FORCE)
set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MI_OVERRIDE ON CACHE BOOL "" FORCE)
set(MI_WIN_REDIRECT ON CACHE BOOL "" FORCE)
set(MI_USE_CXX OFF CACHE STRING "" FORCE)
set(MI_XMALLOC OFF CACHE BOOL "" FORCE)

FetchContent_Declare(mimalloc
    URL "${_mi_url}"
    URL_HASH "SHA256=${_mi_sha256}"
    DOWNLOAD_DIR "${PROJECT_SOURCE_DIR}/.tools/downloads"
    EXCLUDE_FROM_ALL
    SYSTEM)
FetchContent_MakeAvailable(mimalloc)

# Upstream emits diagnostics before returning null, including before main.
# Suppress its default stderr sink rather than relying on late option changes.
find_package(Git REQUIRED)
set(_patch "${PROJECT_SOURCE_DIR}/third_party/mimalloc/atelier.patch")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_patch}")
file(SHA256 "${_patch}" _patch_sha)
set(_stamp "${mimalloc_SOURCE_DIR}/.atelier-patch-sha256")
set(_applied_sha "")
if(EXISTS "${_stamp}")
    file(READ "${_stamp}" _applied_sha)
endif()
if(NOT _applied_sha STREQUAL _patch_sha)
    file(REMOVE "${_stamp}")
    file(MAKE_DIRECTORY "${mimalloc_SOURCE_DIR}/.atelier-upstream")
    set(_original "${mimalloc_SOURCE_DIR}/.atelier-upstream/options.c")
    if(EXISTS "${_original}")
        file(COPY_FILE "${_original}" "${mimalloc_SOURCE_DIR}/src/options.c" ONLY_IF_DIFFERENT)
    else()
        file(COPY_FILE "${mimalloc_SOURCE_DIR}/src/options.c" "${_original}")
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply --check --unsafe-paths "--directory=${mimalloc_SOURCE_DIR}" "${_patch}"
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply --unsafe-paths "--directory=${mimalloc_SOURCE_DIR}" "${_patch}"
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" COMMAND_ERROR_IS_FATAL ANY)
    file(WRITE "${_stamp}" "${_patch_sha}")
endif()

set_target_properties(mimalloc PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
configure_file("${mimalloc_SOURCE_DIR}/LICENSE" "${CMAKE_BINARY_DIR}/LICENSE.mimalloc.txt" COPYONLY)
