# LLVM-facing objects share the pinned SDK's ABI. This helper is private to
# the backend DLLs and build-time Clang tool; other targets retain /MD(d).
function(atelier_llvm_target target)
    atelier_target(${target})
    set_target_properties(${target} PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreaded)
    target_compile_definitions(${target} PRIVATE NDEBUG _ITERATOR_DEBUG_LEVEL=0)
    target_compile_options(${target} PRIVATE /GR-)
    target_include_directories(${target} SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
endfunction()

function(atelier_add_source)
    include(FetchContent)
    file(READ "${PROJECT_SOURCE_DIR}/cmake/dependencies-lock.json" _jit_dependencies)
    foreach(_dependency zlib zstd)
        foreach(_field version url sha256)
            string(JSON _${_dependency}_${_field} GET "${_jit_dependencies}" ${_dependency} ${_field})
        endforeach()
    endforeach()

    set(ZLIB_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZLIB_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(ZLIB_INSTALL OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
    FetchContent_Declare(ZLIB
        URL "${_zlib_url}" URL_HASH "SHA256=${_zlib_sha256}"
        HTTP_HEADER "Accept: application/octet-stream"
        DOWNLOAD_NAME "zlib-${_zlib_version}.tar.gz"
        DOWNLOAD_DIR "${PROJECT_SOURCE_DIR}/.tools/downloads"
        EXCLUDE_FROM_ALL SYSTEM OVERRIDE_FIND_PACKAGE)
    FetchContent_Declare(zstd
        URL "${_zstd_url}" URL_HASH "SHA256=${_zstd_sha256}"
        HTTP_HEADER "Accept: application/octet-stream"
        DOWNLOAD_NAME "zstd-${_zstd_version}.tar.gz"
        DOWNLOAD_DIR "${PROJECT_SOURCE_DIR}/.tools/downloads"
        SOURCE_SUBDIR build/cmake
        EXCLUDE_FROM_ALL SYSTEM OVERRIDE_FIND_PACKAGE)
    FetchContent_MakeAvailable(ZLIB zstd)
    add_library(ZLIB::ZLIB ALIAS zlibstatic)
    if(NOT TARGET zstd::libzstd_static)
        add_library(zstd::libzstd_static ALIAS libzstd_static)
    endif()

    file(TO_CMAKE_PATH "$ENV{ATELIER_LLVM_ROOT}" _sdk)
    find_package(LLVM ${_pin_llvm} EXACT REQUIRED CONFIG
        PATHS "${_sdk}/lib/cmake/llvm" NO_DEFAULT_PATH)
    if(NOT LLVM_BUILD_TYPE STREQUAL "Release" OR
       NOT CMAKE_MSVC_RUNTIME_LIBRARY STREQUAL "MultiThreaded" OR
       LLVM_ENABLE_ASSERTIONS OR LLVM_ENABLE_RTTI)
        message(FATAL_ERROR "Revalidate the pinned LLVM SDK ABI before changing its isolation DLL configuration.")
    endif()
    llvm_map_components_to_libnames(_llvm_libraries Core OrcJIT native AsmParser Passes Coroutines Object MCDisassembler)
    set_target_properties(zlibstatic libzstd_static PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreaded)
    configure_file("${zlib_SOURCE_DIR}/LICENSE" "${CMAKE_BINARY_DIR}/LICENSE.zlib.txt" COPYONLY)
    configure_file("${zstd_SOURCE_DIR}/LICENSE" "${CMAKE_BINARY_DIR}/LICENSE.zstd.txt" COPYONLY)
    # Retain the pinned upstream license without fetching/building compiler-rt.
    configure_file("${PROJECT_SOURCE_DIR}/cmake/LICENSE.llvm.txt" "${CMAKE_BINARY_DIR}/LICENSE.llvm.txt" COPYONLY)

    # Direct objects ensure the pinned replacement wins over LLVMJITLink's
    # archive member. Both DLLs use the same unwind implementation; registration
    # belongs to each code allocation and requires no shared registry.
    add_library(atelier_jit_llvm_support OBJECT
        modules/execution/jit/src/windows_unwind.cpp
        third_party/llvm/JITLinkMemoryManager.cpp
        modules/core/memory/src/global_new_delete.cpp)
    atelier_llvm_target(atelier_jit_llvm_support)
    target_include_directories(atelier_jit_llvm_support PRIVATE "${PROJECT_SOURCE_DIR}/modules/execution/jit/internal")
    target_link_libraries(atelier_jit_llvm_support PUBLIC ${_llvm_libraries} mimalloc)

    # The same frontend is compiled on each side of the isolation boundary;
    # its STL state stays within the host or the LLVM DLL's own CRT ABI.
    add_library(atelier_source_backend SHARED modules/execution/jit/src/source_backend.cpp
        modules/execution/jit/src/diagnostic_artifact.cpp modules/execution/language/src/source.cpp modules/execution/language/src/declarations.cpp modules/execution/language/src/material.cpp)
    atelier_llvm_target(atelier_source_backend)
    target_compile_definitions(atelier_source_backend PRIVATE ATELIER_SOURCE_BACKEND_BUILD)
    # Lowering reads the native transport layout, but does not link Runtime's CRT ABI.
    target_include_directories(atelier_source_backend PRIVATE
        "${PROJECT_SOURCE_DIR}/modules/execution/jit/internal"
        "${PROJECT_SOURCE_DIR}/modules/execution/language/include"
        "${PROJECT_SOURCE_DIR}/modules/execution/runtime/include")
    target_link_libraries(atelier_source_backend PRIVATE atelier_jit_llvm_support unordered_dense::unordered_dense
        atelier_foundation_headers atelier_math atelier_execution_contracts)

    add_library(atelier_source STATIC modules/execution/source/src/program.cpp modules/execution/source/src/diagnostics.cpp modules/execution/source/src/detail/bridge.cpp
        modules/execution/source/src/debugger.cpp modules/execution/source/src/detail/debugger.cpp modules/execution/source/src/snapshot.cpp modules/execution/source/src/material_program.cpp
        modules/execution/language/src/source.cpp modules/execution/language/src/declarations.cpp modules/execution/language/src/module.cpp)
    atelier_target(atelier_source)
    target_include_directories(atelier_source PUBLIC
        "${PROJECT_SOURCE_DIR}/modules/execution/source/include"
        "${PROJECT_SOURCE_DIR}/modules/execution/language/include"
        PRIVATE "${PROJECT_SOURCE_DIR}/modules/execution/source/internal"
        "${PROJECT_SOURCE_DIR}/modules/execution/jit/internal"
        "${PROJECT_SOURCE_DIR}/modules/execution/runtime/internal")
    target_compile_features(atelier_source PUBLIC cxx_std_23)
    target_link_libraries(atelier_source PUBLIC atelier_runtime PRIVATE atelier_source_backend atelier_platform)
    # engine/ beside the executables is an exact copy of assets/engine: every reconfigure starts
    # empty, and adding, removing or editing a .source file reconfigures.
    set(engine_tree "${PROJECT_SOURCE_DIR}/assets/engine")
    target_compile_definitions(atelier_source PRIVATE "ATELIER_ENGINE_SOURCE_TREE=L\"${engine_tree}\"")
    file(REMOVE_RECURSE "${CMAKE_BINARY_DIR}/engine")
    file(GLOB_RECURSE engine_files CONFIGURE_DEPENDS RELATIVE "${engine_tree}" "${engine_tree}/*.source")
    foreach(engine_file IN LISTS engine_files)
        configure_file("${engine_tree}/${engine_file}" "${CMAKE_BINARY_DIR}/engine/${engine_file}" COPYONLY)
    endforeach()

    add_executable(atelier_source_minimal
        acceptance/source/main.cpp acceptance/source/shutdown.cpp acceptance/source/session.cpp
        acceptance/source/inheritance.cpp acceptance/source/native_values.cpp acceptance/source/modules.cpp
        acceptance/source/types.cpp acceptance/source/value_fields.cpp acceptance/source/math.cpp
        acceptance/source/foundations.cpp acceptance/source/plain_records.cpp acceptance/source/projects.cpp
        acceptance/source/diagnostics.cpp acceptance/source/debugger.cpp acceptance/source/materials.cpp)
    atelier_target(atelier_source_minimal)
    include("${PROJECT_SOURCE_DIR}/cmake/core_math.cmake")
    target_link_libraries(atelier_source_minimal PRIVATE atelier_source atelier_world atelier_material atelier_core_math mimalloc psapi)
    target_include_directories(atelier_source_minimal PRIVATE "${PROJECT_SOURCE_DIR}/acceptance"
        "${PROJECT_SOURCE_DIR}/modules/execution/source/internal"
        "${PROJECT_SOURCE_DIR}/modules/execution/jit/internal")
    target_compile_definitions(atelier_source_minimal PRIVATE "ATELIER_ENGINE_SOURCE_TREE=L\"${engine_tree}\"")
    atelier_headless_acceptance(source_acceptance
        COMMAND $<TARGET_FILE:atelier_source_minimal>
        DEPENDS atelier_source_minimal)
    atelier_headless_acceptance(source_material_acceptance
        COMMAND $<TARGET_FILE:atelier_source_minimal> --materials
        DEPENDS atelier_source_minimal)

    add_library(atelier_jit_probe SHARED acceptance/jit/backend.cpp)
    atelier_llvm_target(atelier_jit_probe)
    target_include_directories(atelier_jit_probe PRIVATE "${PROJECT_SOURCE_DIR}/modules/execution/jit/internal")
    target_compile_definitions(atelier_jit_probe PRIVATE ATELIER_JIT_PROBE_BUILD)
    target_link_libraries(atelier_jit_probe PRIVATE atelier_jit_llvm_support)

    add_library(atelier_jit_native_client SHARED acceptance/jit/native_client.cpp)
    target_compile_definitions(atelier_jit_native_client PRIVATE ATELIER_JIT_NATIVE_CLIENT_BUILD)
    atelier_target(atelier_jit_native_client)

    add_executable(atelier_jit_acceptance acceptance/jit/main.cpp acceptance/jit/business.cpp
        acceptance/jit/math_abi.cpp)
    atelier_target(atelier_jit_acceptance)
    target_include_directories(atelier_jit_acceptance PRIVATE acceptance
        "${PROJECT_SOURCE_DIR}/modules/execution/jit/internal"
        "${PROJECT_SOURCE_DIR}/modules/execution/source/internal")
    target_link_libraries(atelier_jit_acceptance PRIVATE
        atelier_source atelier_source_backend atelier_jit_probe atelier_jit_native_client)
    atelier_headless_acceptance(jit_acceptance
        COMMAND $<TARGET_FILE:atelier_jit_acceptance>
        DEPENDS source_acceptance atelier_jit_acceptance)
endfunction()
