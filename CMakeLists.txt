cmake_minimum_required(VERSION 4.2)
project(FrostFoxAtelier VERSION 0.1.0 LANGUAGES C CXX)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchain-lock.json" _lock)
foreach(_tool llvm cmake ninja msvc windowsSdk)
    string(JSON _pin_${_tool} GET "${_lock}" "${_tool}")
endforeach()
if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR
   NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
   NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" OR
   NOT CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL _pin_llvm OR
   NOT CMAKE_C_COMPILER_ID STREQUAL "Clang" OR
   NOT CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" OR
   NOT CMAKE_C_COMPILER_VERSION VERSION_EQUAL _pin_llvm)
    message(FATAL_ERROR "Atelier requires Windows x64 and clang-cl ${_pin_llvm}.")
endif()
if(NOT CMAKE_VERSION STREQUAL _pin_cmake OR NOT CMAKE_GENERATOR STREQUAL "Ninja")
    message(FATAL_ERROR "Atelier requires CMake ${_pin_cmake} with Ninja.")
endif()
execute_process(COMMAND "${CMAKE_MAKE_PROGRAM}" --version OUTPUT_VARIABLE _ninja OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_LINKER}" --version OUTPUT_VARIABLE _lld OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT _ninja STREQUAL _pin_ninja OR NOT _lld MATCHES "^LLD ${_pin_llvm}($| )")
    message(FATAL_ERROR "Atelier requires Ninja ${_pin_ninja} and LLD ${_pin_llvm}.")
endif()
string(REGEX REPLACE "[/\\]+$" "" _msvc "$ENV{VCToolsVersion}")
string(REGEX REPLACE "[/\\]+$" "" _sdk "$ENV{WindowsSDKVersion}")
if(NOT _msvc STREQUAL _pin_msvc OR NOT _sdk STREQUAL _pin_windowsSdk)
    message(FATAL_ERROR "Use tools/enter-dev.ps1 to select the pinned MSVC/SDK environment.")
endif()

include(cmake/mimalloc.cmake)
include(cmake/acceptance.cmake)

add_library(atelier_memory_headers INTERFACE)
target_include_directories(atelier_memory_headers INTERFACE modules/core/memory/include)

function(atelier_target target)
    cmake_parse_arguments(PARSE_ARGV 1 _arg "CRT_ALLOCATOR_PROBE" "" "")
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    target_compile_options(${target} PRIVATE /W4 /WX /EHsc /permissive- /utf-8)
    target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(${target} PRIVATE atelier_memory_headers)
    get_target_property(_type ${target} TYPE)
    if(_type STREQUAL "EXECUTABLE" OR _type STREQUAL "SHARED_LIBRARY")
        # Compile in the owning CRT domain, including LLVM's /MT targets.
        target_sources(${target} PRIVATE "${PROJECT_SOURCE_DIR}/modules/core/memory/src/fatal.cpp")
    endif()
    if((_type STREQUAL "EXECUTABLE" OR _type STREQUAL "SHARED_LIBRARY") AND NOT _arg_CRT_ALLOCATOR_PROBE)
        # Link the overrides directly: an unreferenced static-library object can be discarded.
        target_sources(${target} PRIVATE "${PROJECT_SOURCE_DIR}/modules/core/memory/src/global_new_delete.cpp")
        target_link_libraries(${target} PRIVATE mimalloc)
        target_link_options(${target} PRIVATE /include:mi_version)
    endif()
endfunction()

include(cmake/logging.cmake)
include(cmake/unordered_dense.cmake)
include(cmake/tbb.cmake)
add_library(atelier_foundation_headers INTERFACE)
target_include_directories(atelier_foundation_headers INTERFACE modules/core/foundation/include)
target_link_libraries(atelier_foundation_headers INTERFACE unordered_dense::unordered_dense atelier_memory_headers mimalloc)

add_library(atelier_math INTERFACE)
target_include_directories(atelier_math INTERFACE modules/core/math/include)
# Numerical consumers rely on separate multiply/add rounding (notably Volume/BVH bounds).
target_compile_options(atelier_math INTERFACE /clang:-ffp-contract=off)

add_library(atelier_platform INTERFACE)
target_include_directories(atelier_platform INTERFACE modules/core/platform/include)

add_library(atelier_execution_contracts INTERFACE)
target_include_directories(atelier_execution_contracts INTERFACE modules/execution/contracts/include)

add_library(atelier_foundation STATIC modules/core/foundation/src/string.cpp modules/core/foundation/src/hash.cpp modules/core/foundation/src/frame_timing.cpp
    modules/core/foundation/src/sha256.cpp modules/core/foundation/src/tasks.cpp)
target_compile_features(atelier_foundation PUBLIC cxx_std_23)
target_link_libraries(atelier_foundation PUBLIC atelier_foundation_headers PRIVATE atelier_platform bcrypt TBB::tbb)
target_compile_definitions(atelier_foundation PRIVATE TBB_USE_EXCEPTIONS=0)
atelier_target(atelier_foundation)

add_executable(atelier_math_acceptance acceptance/math/main.cpp)
atelier_target(atelier_math_acceptance)
target_link_libraries(atelier_math_acceptance PRIVATE atelier_math)
atelier_headless_acceptance(math_acceptance
    COMMAND $<TARGET_FILE:atelier_math_acceptance>
    DEPENDS atelier_math_acceptance)

add_library(atelier_runtime STATIC modules/execution/runtime/src/runtime.cpp modules/execution/runtime/src/execution.cpp)
target_include_directories(atelier_runtime PUBLIC modules/execution/runtime/include
    PRIVATE modules/execution/runtime/internal)
target_compile_features(atelier_runtime PUBLIC cxx_std_23)
atelier_target(atelier_runtime)
target_link_libraries(atelier_runtime PUBLIC atelier_foundation atelier_math atelier_execution_contracts)

add_library(atelier_allocator_client SHARED acceptance/memory/crt_client.cpp)
target_compile_definitions(atelier_allocator_client PRIVATE ATELIER_ALLOCATOR_CLIENT_BUILD)
# Keep the CRT malloc/realloc redirector control independent of direct mimalloc links.
atelier_target(atelier_allocator_client CRT_ALLOCATOR_PROBE)

add_executable(atelier_runtime_acceptance
    acceptance/runtime/main.cpp
    acceptance/runtime/inheritance.cpp
    acceptance/runtime/reference_sequence.cpp
    acceptance/runtime/field_ranges.cpp
    acceptance/runtime/arrays.cpp
    acceptance/runtime/value_paths.cpp
    acceptance/runtime/write_stamps.cpp
    acceptance/memory/acceptance.cpp)
atelier_target(atelier_runtime_acceptance)
target_include_directories(atelier_runtime_acceptance PRIVATE acceptance)
target_link_libraries(atelier_runtime_acceptance PRIVATE atelier_runtime atelier_allocator_client)
atelier_headless_acceptance(runtime_acceptance
    COMMAND $<TARGET_FILE:atelier_runtime_acceptance>
    DEPENDS atelier_runtime_acceptance)

add_library(atelier_ui_layout STATIC modules/gui/ui_layout/src/layout.cpp)
target_include_directories(atelier_ui_layout PUBLIC modules/gui/ui_layout/include)
target_link_libraries(atelier_ui_layout PRIVATE atelier_foundation_headers)
target_compile_features(atelier_ui_layout PUBLIC cxx_std_23)
atelier_target(atelier_ui_layout)

add_executable(atelier_ui_layout_acceptance acceptance/ui_layout/main.cpp)
atelier_target(atelier_ui_layout_acceptance)
target_link_libraries(atelier_ui_layout_acceptance PRIVATE atelier_ui_layout dwrite)
atelier_headless_acceptance(ui_layout_acceptance
    COMMAND $<TARGET_FILE:atelier_ui_layout_acceptance>
    DEPENDS atelier_ui_layout_acceptance)

include(cmake/jit.cmake)
atelier_add_source()

include(cmake/reflection.cmake)
include(cmake/style.cmake)
include(cmake/foundation.cmake)

add_library(atelier_material STATIC
    modules/engine/material/src/program.cpp
    modules/engine/material/src/program_resources.cpp
    modules/engine/material/src/resources.cpp)
atelier_target(atelier_material)
target_include_directories(atelier_material PUBLIC modules/engine/material/include)
target_compile_features(atelier_material PUBLIC cxx_std_23)
target_link_libraries(atelier_material PUBLIC atelier_runtime atelier_source)
atelier_reflect(atelier_material SOURCE modules/engine/material/src/resources.cpp
    BASE_DIR modules/engine/material/include HEADERS modules/engine/material/include/material/resources.hpp)

add_executable(atelier_material_program_resources_acceptance acceptance/material/program_resources.cpp)
atelier_target(atelier_material_program_resources_acceptance)
target_link_libraries(atelier_material_program_resources_acceptance PRIVATE atelier_material)
atelier_headless_acceptance(material_program_resources_acceptance
    COMMAND $<TARGET_FILE:atelier_material_program_resources_acceptance>
    DEPENDS atelier_material_program_resources_acceptance)

include(cmake/geometry.cmake)
add_library(atelier_rendering STATIC modules/engine/rendering/src/sky_map.cpp)
atelier_target(atelier_rendering)
target_include_directories(atelier_rendering PUBLIC modules/engine/rendering/include)
target_link_libraries(atelier_rendering PUBLIC atelier_math)

include(cmake/procedural.cmake)
atelier_add_procedural()

include(cmake/world.cmake)
atelier_add_world()

include(cmake/jolt.cmake)
atelier_add_jolt()

include(cmake/rhi.cmake)
atelier_add_rhi()

include(cmake/ui.cmake)
atelier_add_ui()

include(cmake/renderer.cmake)
atelier_add_renderer()

include(cmake/editor.cmake)
atelier_add_editor()

get_property(_headless_acceptance GLOBAL PROPERTY ATELIER_HEADLESS_ACCEPTANCE)
get_property(_renderer_acceptance GLOBAL PROPERTY ATELIER_RENDERER_ACCEPTANCE)
# GPU creation and native input belong here even when a target starts with CPU checks.
# USES_TERMINAL on these leaf targets serializes them in Ninja's console pool.
set(_window_acceptance
    rhi_acceptance ${_renderer_acceptance} editor_acceptance
    editor_control_acceptance editor_control_path_acceptance editor_project_acceptance
    editor_repro_material_acceptance editor_output_acceptance editor_log_smoke_acceptance
    editor_shortcuts_acceptance editor_edit_acceptance editor_surfaces_acceptance
    editor_dialogs_acceptance editor_scene_lit_acceptance)
add_custom_target(acceptance_headless DEPENDS ${_headless_acceptance})
add_custom_target(acceptance_window DEPENDS ${_window_acceptance})
add_custom_target(acceptance DEPENDS acceptance_headless acceptance_window)
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/acceptance-window-targets.txt"
    CONTENT "$<JOIN:${_window_acceptance},\n>\n")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/acceptance-headless-targets.txt"
    CONTENT "$<JOIN:${_headless_acceptance},\n>\n")
