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

function(atelier_target target)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    target_compile_options(${target} PRIVATE /W4 /WX /EHsc /permissive- /utf-8)
    target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    get_target_property(_type ${target} TYPE)
    if(_type STREQUAL "EXECUTABLE")
        # Link the overrides directly: an unreferenced static-library object can be discarded.
        target_sources(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src/memory/global_new_delete.cpp")
        target_link_libraries(${target} PRIVATE mimalloc)
        target_link_options(${target} PRIVATE /include:mi_version)
    endif()
endfunction()

include(cmake/unordered_dense.cmake)
add_library(atelier_foundation STATIC src/foundation/string.cpp src/foundation/hash.cpp src/foundation/frame_timing.cpp)
target_include_directories(atelier_foundation PUBLIC src)
target_compile_features(atelier_foundation PUBLIC cxx_std_23)
target_link_libraries(atelier_foundation PUBLIC unordered_dense::unordered_dense)
atelier_target(atelier_foundation)

add_executable(atelier_math_acceptance acceptance/math/main.cpp)
atelier_target(atelier_math_acceptance)
target_include_directories(atelier_math_acceptance PRIVATE src)
atelier_headless_acceptance(math_acceptance
    COMMAND $<TARGET_FILE:atelier_math_acceptance>
    DEPENDS atelier_math_acceptance)

add_library(atelier_runtime STATIC src/runtime/runtime.cpp src/runtime/execution.cpp)
target_include_directories(atelier_runtime PUBLIC src)
target_compile_features(atelier_runtime PUBLIC cxx_std_23)
atelier_target(atelier_runtime)
target_link_libraries(atelier_runtime PUBLIC atelier_foundation)

add_library(atelier_allocator_client SHARED acceptance/memory/crt_client.cpp)
target_compile_definitions(atelier_allocator_client PRIVATE ATELIER_ALLOCATOR_CLIENT_BUILD)
atelier_target(atelier_allocator_client)

add_executable(atelier_runtime_acceptance
    acceptance/runtime/main.cpp
    acceptance/runtime/inheritance.cpp
    acceptance/runtime/reference_sequence.cpp
    acceptance/runtime/math_precision.cpp
    acceptance/runtime/field_ranges.cpp
    acceptance/runtime/validation.cpp
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

add_library(atelier_ui_layout STATIC src/ui_layout/layout.cpp)
target_include_directories(atelier_ui_layout PUBLIC src)
target_compile_features(atelier_ui_layout PUBLIC cxx_std_23)
atelier_target(atelier_ui_layout)

add_executable(atelier_ui_layout_minimal acceptance/ui_layout/minimal.cpp)
atelier_target(atelier_ui_layout_minimal)
target_link_libraries(atelier_ui_layout_minimal PRIVATE atelier_ui_layout)

add_executable(atelier_ui_layout_acceptance acceptance/ui_layout/main.cpp)
atelier_target(atelier_ui_layout_acceptance)
target_link_libraries(atelier_ui_layout_acceptance PRIVATE atelier_ui_layout dwrite)
atelier_headless_acceptance(ui_layout_acceptance
    COMMAND $<TARGET_FILE:atelier_ui_layout_minimal>
    COMMAND $<TARGET_FILE:atelier_ui_layout_acceptance>
    DEPENDS atelier_ui_layout_minimal atelier_ui_layout_acceptance)

include(cmake/jit.cmake)
atelier_add_source()

include(cmake/reflection.cmake)
include(cmake/foundation.cmake)

include(cmake/geometry.cmake)

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
    editor_shortcuts_acceptance editor_edit_acceptance editor_surfaces_acceptance
    editor_dialogs_acceptance editor_scene_lit_acceptance physics_examples_acceptance
    editor_sponza_acceptance editor_examples_acceptance editor_dungeons_acceptance)
add_custom_target(acceptance_headless DEPENDS ${_headless_acceptance})
add_custom_target(acceptance_window DEPENDS ${_window_acceptance})
add_custom_target(acceptance DEPENDS acceptance_headless acceptance_window)
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/acceptance-window-targets.txt"
    CONTENT "$<JOIN:${_window_acceptance},\n>\n")
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/acceptance-headless-targets.txt"
    CONTENT "$<JOIN:${_headless_acceptance},\n>\n")
