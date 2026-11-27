include_guard(GLOBAL)
include(FetchContent)

file(READ "${PROJECT_SOURCE_DIR}/cmake/dependencies-lock.json" _assets_lock)
string(JSON _bc7_commit GET "${_assets_lock}" bc7enc_rdo commit)
string(SUBSTRING "${_bc7_commit}" 0 8 _bc7_short)
string(JSON _bc7_url GET "${_assets_lock}" bc7enc_rdo url)
string(JSON _bc7_sha GET "${_assets_lock}" bc7enc_rdo sha256)
string(JSON _bc7_file GET "${_assets_lock}" bc7enc_rdo filename)
set(_bc7_dir "${CMAKE_BINARY_DIR}/_deps/bc7enc-${_bc7_short}")

FetchContent_Declare(atelier_dep_bc7enc
    URL "${_bc7_url}" URL_HASH "SHA256=${_bc7_sha}"
    DOWNLOAD_NAME "${_bc7_file}"
    DOWNLOAD_DIR "${PROJECT_SOURCE_DIR}/.tools/downloads"
    SOURCE_DIR "${_bc7_dir}"
    SOURCE_SUBDIR "atelier_ignore_upstream_cmakelists"
    EXCLUDE_FROM_ALL SYSTEM)
FetchContent_MakeAvailable(atelier_dep_bc7enc)

add_library(atelier_bc7enc STATIC
    "${_bc7_dir}/bc7enc.cpp"
    "${_bc7_dir}/rgbcx.cpp")
set_target_properties(atelier_bc7enc PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
target_include_directories(atelier_bc7enc SYSTEM PUBLIC "${_bc7_dir}")
target_compile_definitions(atelier_bc7enc PRIVATE _HAS_EXCEPTIONS=0 _CRT_SECURE_NO_WARNINGS)
target_compile_options(atelier_bc7enc PRIVATE /utf-8 /EHs-c- -w)

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/licenses")
configure_file("${_bc7_dir}/LICENSE" "${CMAKE_BINARY_DIR}/licenses/LICENSE.bc7enc.txt" COPYONLY)

add_library(atelier_assets STATIC
    "${PROJECT_SOURCE_DIR}/modules/engine/assets/src/atx_texture.cpp"
    "${PROJECT_SOURCE_DIR}/modules/engine/assets/src/builtin.cpp"
    "${PROJECT_SOURCE_DIR}/modules/engine/assets/src/builtin_materials.cpp"
    "${PROJECT_SOURCE_DIR}/modules/engine/assets/src/content_digest.cpp"
    "${PROJECT_SOURCE_DIR}/modules/engine/assets/src/reader.cpp"
    "${PROJECT_SOURCE_DIR}/modules/engine/assets/src/file.cpp"
    "${PROJECT_SOURCE_DIR}/modules/engine/assets/src/image_decode.cpp"
    "${PROJECT_SOURCE_DIR}/modules/engine/assets/src/png.cpp"
    "${PROJECT_SOURCE_DIR}/modules/engine/assets/src/obj_import.cpp"
    "${PROJECT_SOURCE_DIR}/modules/engine/assets/src/gltf_import.cpp")
atelier_target(atelier_assets)
target_include_directories(atelier_assets PUBLIC "${PROJECT_SOURCE_DIR}/modules/engine/assets/include")
# No editor, presentation, physics, Renderer or device dependency belongs here.
target_link_libraries(atelier_assets PUBLIC atelier_runtime atelier_geometry atelier_rendering atelier_source atelier_material
    atelier_platform atelier_core_json PRIVATE atelier_foundation atelier_bc7enc windowscodecs ole32)

target_sources(atelier_world_acceptance PRIVATE "${PROJECT_SOURCE_DIR}/acceptance/assets/mesh_import.cpp")
target_link_libraries(atelier_world_acceptance PRIVATE atelier_assets)
