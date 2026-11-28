include_guard(GLOBAL)
include(FetchContent)

file(READ "${PROJECT_SOURCE_DIR}/cmake/dependencies-lock.json" _geo_lock)
string(JSON _mopt_ver GET "${_geo_lock}" meshoptimizer version)
string(JSON _mopt_url GET "${_geo_lock}" meshoptimizer url)
string(JSON _mopt_sha GET "${_geo_lock}" meshoptimizer sha256)
string(JSON _mopt_file GET "${_geo_lock}" meshoptimizer filename)
set(_mopt_dir "${CMAKE_BINARY_DIR}/_deps/meshoptimizer-${_mopt_ver}")

FetchContent_Declare(atelier_dep_meshoptimizer
    URL "${_mopt_url}" URL_HASH "SHA256=${_mopt_sha}"
    DOWNLOAD_NAME "${_mopt_file}"
    DOWNLOAD_DIR "${PROJECT_SOURCE_DIR}/.tools/downloads"
    SOURCE_DIR "${_mopt_dir}"
    SOURCE_SUBDIR "atelier_ignore_upstream_cmakelists"
    EXCLUDE_FROM_ALL SYSTEM)
FetchContent_MakeAvailable(atelier_dep_meshoptimizer)

add_library(atelier_meshoptimizer STATIC
    "${_mopt_dir}/src/allocator.cpp"
    "${_mopt_dir}/src/clusterizer.cpp"
    "${_mopt_dir}/src/indexgenerator.cpp"
    "${_mopt_dir}/src/meshletutils.cpp"
    "${_mopt_dir}/src/overdrawoptimizer.cpp"
    "${_mopt_dir}/src/partition.cpp"
    "${_mopt_dir}/src/quantization.cpp"
    "${_mopt_dir}/src/simplifier.cpp"
    "${_mopt_dir}/src/spatialorder.cpp"
    "${_mopt_dir}/src/vcacheoptimizer.cpp"
    "${_mopt_dir}/src/vfetchoptimizer.cpp")
set_target_properties(atelier_meshoptimizer PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
target_include_directories(atelier_meshoptimizer SYSTEM PUBLIC "${_mopt_dir}/src")
target_compile_definitions(atelier_meshoptimizer PRIVATE _HAS_EXCEPTIONS=0)
target_compile_options(atelier_meshoptimizer PRIVATE /utf-8 /EHs-c- -w)

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/licenses")
configure_file("${_mopt_dir}/LICENSE.md" "${CMAKE_BINARY_DIR}/licenses/LICENSE.meshoptimizer.txt" COPYONLY)

add_library(atelier_mikktspace STATIC "${PROJECT_SOURCE_DIR}/third_party/mikktspace/mikktspace.c")
set_target_properties(atelier_mikktspace PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
target_include_directories(atelier_mikktspace PUBLIC "${PROJECT_SOURCE_DIR}/third_party/mikktspace")

add_library(atelier_geometry STATIC
    "${PROJECT_SOURCE_DIR}/modules/engine/geometry/src/tangents.cpp"
    "${PROJECT_SOURCE_DIR}/modules/engine/geometry/src/meshlet.cpp")
atelier_target(atelier_geometry)
target_include_directories(atelier_geometry PUBLIC "${PROJECT_SOURCE_DIR}/modules/engine/geometry/include")
target_link_libraries(atelier_geometry
    PUBLIC atelier_math atelier_foundation
    PRIVATE atelier_platform atelier_mikktspace atelier_meshoptimizer)
