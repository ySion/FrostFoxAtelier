include(FetchContent)

file(READ "${CMAKE_CURRENT_LIST_DIR}/dependencies-lock.json" _tbb_dependencies)
string(JSON _tbb_url GET "${_tbb_dependencies}" oneTBB url)
string(JSON _tbb_sha GET "${_tbb_dependencies}" oneTBB sha256)
string(JSON _tbb_version GET "${_tbb_dependencies}" oneTBB version)
set(TBB_TEST OFF CACHE BOOL "" FORCE)
set(TBB_EXAMPLES OFF CACHE BOOL "" FORCE)
set(TBBMALLOC_BUILD OFF CACHE BOOL "" FORCE)
set(TBBMALLOC_PROXY_BUILD OFF CACHE BOOL "" FORCE)
set(TCM_BUILD OFF CACHE BOOL "" FORCE)
set(TBB_INSTALL OFF CACHE BOOL "" FORCE)
set(TBB_DISABLE_HWLOC_AUTOMATIC_SEARCH ON CACHE BOOL "" FORCE)

# Only one scheduler DLL per process. Do not change other dependencies' linkage.
block(SCOPE_FOR VARIABLES)
    set(BUILD_SHARED_LIBS ON)
    FetchContent_Declare(atelier_tbb
        URL "${_tbb_url}" URL_HASH "SHA256=${_tbb_sha}"
        DOWNLOAD_NAME "oneTBB-v${_tbb_version}.tar.gz"
        DOWNLOAD_DIR "${PROJECT_SOURCE_DIR}/.tools/downloads" EXCLUDE_FROM_ALL SYSTEM)
    FetchContent_MakeAvailable(atelier_tbb)
    set_target_properties(tbb PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    configure_file("${atelier_tbb_SOURCE_DIR}/LICENSE.txt"
        "${CMAKE_BINARY_DIR}/LICENSE.oneTBB.txt" COPYONLY)
endblock()
