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
set(MI_USE_CXX ON CACHE STRING "" FORCE)
set(MI_XMALLOC OFF CACHE BOOL "" FORCE)

FetchContent_Declare(mimalloc
    URL "${_mi_url}"
    URL_HASH "SHA256=${_mi_sha256}"
    DOWNLOAD_DIR "${PROJECT_SOURCE_DIR}/.tools/downloads"
    EXCLUDE_FROM_ALL
    SYSTEM)
FetchContent_MakeAvailable(mimalloc)
set_target_properties(mimalloc PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
configure_file("${mimalloc_SOURCE_DIR}/LICENSE" "${CMAKE_BINARY_DIR}/LICENSE.mimalloc.txt" COPYONLY)
