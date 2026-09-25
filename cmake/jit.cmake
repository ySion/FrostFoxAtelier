# Keep the prebuilt LLVM C++ ABI private to the platform-acceptance DLL.
# Other project targets retain atelier_target's Debug/Release CRT contract.
function(atelier_add_jit_acceptance)
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
    llvm_map_components_to_libnames(_llvm_libraries Core OrcJIT native AsmParser)
    set_target_properties(zlibstatic libzstd_static PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreaded)
    configure_file("${zlib_SOURCE_DIR}/LICENSE" "${CMAKE_BINARY_DIR}/LICENSE.zlib.txt" COPYONLY)
    configure_file("${zstd_SOURCE_DIR}/LICENSE" "${CMAKE_BINARY_DIR}/LICENSE.zstd.txt" COPYONLY)
    # Retain the pinned upstream license without fetching/building compiler-rt.
    configure_file("${PROJECT_SOURCE_DIR}/cmake/LICENSE.llvm.txt" "${CMAKE_BINARY_DIR}/LICENSE.llvm.txt" COPYONLY)

    add_library(atelier_jit_probe SHARED
        acceptance/jit/backend.cpp
        acceptance/jit/business_backend.cpp
        src/language/source.cpp
        src/jit/windows_unwind.cpp)
    atelier_target(atelier_jit_probe)
    # Match the SDK, not the calling executable: no LLVM/STL object or
    # exception crosses backend.hpp. Runtime acceptance still uses /MDd.
    set_target_properties(atelier_jit_probe PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreaded)
    target_compile_definitions(atelier_jit_probe PRIVATE
        ATELIER_JIT_PROBE_BUILD NDEBUG _ITERATOR_DEBUG_LEVEL=0)
    target_compile_options(atelier_jit_probe PRIVATE /GR-)
    target_include_directories(atelier_jit_probe SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
    target_include_directories(atelier_jit_probe PRIVATE src)
    target_link_libraries(atelier_jit_probe PRIVATE ${_llvm_libraries} mimalloc)
    target_sources(atelier_jit_probe PRIVATE src/memory/global_new_delete.cpp)
    # Compile the complete pinned replacement object so LLVMJITLink's unpatched
    # archive member is not selected. See its provenance and version guard.
    target_sources(atelier_jit_probe PRIVATE third_party/llvm/JITLinkMemoryManager.cpp)
    configure_file("${PROJECT_SOURCE_DIR}/acceptance/jit/advance_machine.source"
        "${CMAKE_BINARY_DIR}/advance_machine.source" COPYONLY)

    add_executable(atelier_jit_acceptance acceptance/jit/main.cpp acceptance/jit/business.cpp)
    atelier_target(atelier_jit_acceptance)
    target_include_directories(atelier_jit_acceptance PRIVATE acceptance)
    target_link_libraries(atelier_jit_acceptance PRIVATE atelier_runtime atelier_jit_probe atelier_jit_native_client)
    add_custom_target(jit_acceptance
        COMMAND $<TARGET_FILE:atelier_jit_acceptance>
        DEPENDS atelier_jit_acceptance USES_TERMINAL VERBATIM)
endfunction()
