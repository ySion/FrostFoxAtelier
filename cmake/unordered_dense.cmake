include(FetchContent)
file(READ "${CMAKE_CURRENT_LIST_DIR}/dependencies-lock.json" _dense_dependencies)
string(JSON _dense_url GET "${_dense_dependencies}" unordered_dense url)
string(JSON _dense_sha GET "${_dense_dependencies}" unordered_dense sha256)
string(JSON _dense_version GET "${_dense_dependencies}" unordered_dense version)
set(ANKERL_ENABLE_MODULES OFF CACHE BOOL "" FORCE)
set(ANKERL_ENABLE_EXAMPLE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(atelier_unordered_dense
    URL "${_dense_url}" URL_HASH "SHA256=${_dense_sha}"
    DOWNLOAD_NAME "unordered_dense-v${_dense_version}.tar.gz"
    DOWNLOAD_DIR "${PROJECT_SOURCE_DIR}/.tools/downloads" EXCLUDE_FROM_ALL SYSTEM)
FetchContent_MakeAvailable(atelier_unordered_dense)
configure_file("${atelier_unordered_dense_SOURCE_DIR}/LICENSE"
    "${CMAKE_BINARY_DIR}/LICENSE.unordered_dense.txt" COPYONLY)
