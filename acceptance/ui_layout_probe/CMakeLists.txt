cmake_minimum_required(VERSION 4.2)
project(FrostFoxLayoutProbe LANGUAGES C CXX)
get_filename_component(ATELIER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(LP_DEPS_ROOT "${ATELIER_ROOT}/out/review/ui-layout-validation/vendor" CACHE PATH "Pinned experimental sources")
set(LP_CARGO_TARGET "${ATELIER_ROOT}/out/review/ui-layout-validation/cargo" CACHE PATH "Isolated Cargo outputs")
set(LP_CANDIDATES "yoga;taffy;clay" CACHE STRING "Candidate executables to build")
include("${ATELIER_ROOT}/cmake/mimalloc.cmake")

function(layout_probe target)
  add_executable(${target} main.cpp ${ARGN} "${ATELIER_ROOT}/src/memory/global_new_delete.cpp")
  set_target_properties(${target} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  target_compile_options(${target} PRIVATE /W4 /WX /EHsc /permissive- /utf-8)
  target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_link_libraries(${target} PRIVATE mimalloc dwrite)
  target_link_options(${target} PRIVATE /include:mi_version)
  set(LP_TARGETS ${LP_TARGETS} ${target} PARENT_SCOPE)
endfunction()
if("yoga" IN_LIST LP_CANDIDATES)
add_subdirectory("${LP_DEPS_ROOT}/yoga/yoga" "${CMAKE_CURRENT_BINARY_DIR}/yoga" EXCLUDE_FROM_ALL)
layout_probe(layout_probe_yoga yoga_adapter.cpp)
target_link_libraries(layout_probe_yoga PRIVATE yogacore)
endif()
if("clay" IN_LIST LP_CANDIDATES)
layout_probe(layout_probe_clay clay_adapter.cpp)
target_include_directories(layout_probe_clay SYSTEM PRIVATE "${LP_DEPS_ROOT}/clay")
endif()

if("taffy" IN_LIST LP_CANDIDATES)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(_cargo_profile debug)
  set(_cargo_flags)
  set(_mi_import_name mimalloc-debug)
else()
  set(_cargo_profile release)
  set(_cargo_flags --release)
  set(_mi_import_name mimalloc)
endif()
set(_rust_dir "${LP_CARGO_TARGET}/${_cargo_profile}")
set(_rust_dll "${_rust_dir}/atelier_layout_probe_taffy.dll")
set(_rust_import "${_rust_dir}/atelier_layout_probe_taffy.dll.lib")
set(_rust_native "${CMAKE_BINARY_DIR}/rust-native")
add_custom_command(OUTPUT "${_rust_dll}" "${_rust_import}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${_rust_native}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_LINKER_FILE:mimalloc>" "${_rust_native}/${_mi_import_name}.lib"
  COMMAND "${CMAKE_COMMAND}" -E env "CARGO_TARGET_DIR=${LP_CARGO_TARGET}" "RUSTFLAGS=-L native=${_rust_native}" cargo build
    --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/taffy/Cargo.toml" --locked ${_cargo_flags}
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/taffy/Cargo.toml"
    "${CMAKE_CURRENT_SOURCE_DIR}/taffy/Cargo.lock" "${CMAKE_CURRENT_SOURCE_DIR}/taffy/src/lib.rs"
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/taffy" VERBATIM)
add_custom_target(layout_probe_taffy_rust DEPENDS "${_rust_dll}" "${_rust_import}")
add_dependencies(layout_probe_taffy_rust mimalloc)
add_library(layout_probe_taffy_library SHARED IMPORTED)
set_target_properties(layout_probe_taffy_library PROPERTIES IMPORTED_LOCATION "${_rust_dll}" IMPORTED_IMPLIB "${_rust_import}")
add_dependencies(layout_probe_taffy_library layout_probe_taffy_rust)
layout_probe(layout_probe_taffy)
target_link_libraries(layout_probe_taffy PRIVATE layout_probe_taffy_library)
add_custom_command(TARGET layout_probe_taffy POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
  "${_rust_dll}" "${CMAKE_BINARY_DIR}/atelier_layout_probe_taffy.dll")
endif()
add_custom_target(layout_probes DEPENDS ${LP_TARGETS})
