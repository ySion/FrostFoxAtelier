include(FetchContent)

function(atelier_gfx_dependency name key source_dir)
    file(READ "${PROJECT_SOURCE_DIR}/cmake/dependencies-lock.json" _lock)
    foreach(_field url sha256 filename)
        string(JSON _${_field} GET "${_lock}" "${key}" "${_field}")
    endforeach()
    # These declarations precede upstream's: the engine lock remains authoritative.
    FetchContent_Declare(${name}
        URL "${_url}" URL_HASH "SHA256=${_sha256}"
        DOWNLOAD_NAME "${_filename}"
        DOWNLOAD_DIR "${PROJECT_SOURCE_DIR}/.tools/downloads"
        SOURCE_DIR "${source_dir}"
        EXCLUDE_FROM_ALL SYSTEM)
endfunction()

function(atelier_patch_gfx source_dir)
    find_package(Git REQUIRED)
    set(_patch "${PROJECT_SOURCE_DIR}/third_party/gfx/atelier.patch")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_patch}")
    file(SHA256 "${_patch}" _patch_sha)
    set(_stamp "${source_dir}/.atelier-patch-sha256")
    if(EXISTS "${_stamp}")
        file(READ "${_stamp}" _applied_sha)
        if(_applied_sha STREQUAL _patch_sha)
            return()
        endif()
    endif()
    # Only generated dependency sources are restored. Keep a pristine copy so
    # editing our patch never stacks it on an older patched implementation.
    file(REMOVE "${_stamp}")
    file(MAKE_DIRECTORY "${source_dir}/.atelier-upstream")
    foreach(_file gfx.cpp gfx.h gfx_core.h gfx_core.cpp)
        set(_original "${source_dir}/.atelier-upstream/${_file}")
        if(EXISTS "${_original}")
            file(COPY_FILE "${_original}" "${source_dir}/${_file}" ONLY_IF_DIFFERENT)
        else()
            file(COPY_FILE "${source_dir}/${_file}" "${_original}")
        endif()
    endforeach()
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply --check "${_patch}"
        WORKING_DIRECTORY "${source_dir}" RESULT_VARIABLE _check
        OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _check EQUAL 0)
        message(FATAL_ERROR "Pinned gfx patch does not apply: ${_out}${_err}")
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply "${_patch}"
        WORKING_DIRECTORY "${source_dir}" COMMAND_ERROR_IS_FATAL ANY)
    file(WRITE "${_stamp}" "${_patch_sha}")
endfunction()

function(atelier_add_rhi)
    set(_gfx "${CMAKE_BINARY_DIR}/_deps/gfx-src")
    atelier_gfx_dependency(gfx gfx "${_gfx}")
    atelier_gfx_dependency(directx-dxc gfx_dxc "${_gfx}/third_party/directx-dxc")
    atelier_gfx_dependency(DirectX12-Agility gfx_agility "${_gfx}/third_party/directx12-agility")
    atelier_gfx_dependency(D3D12MemoryAllocator gfx_d3d12ma "${_gfx}/third_party/D3D12MemoryAllocator")
    atelier_gfx_dependency(winpixevent gfx_pix "${_gfx}/third_party/WinPixEventRuntime")
    set(GFX_BUILD_EXAMPLES OFF CACHE BOOL "No vendor examples in the engine" FORCE)
    set(GFX_ENABLE_GUI OFF CACHE BOOL "UI belongs to its own module" FORCE)
    set(GFX_ENABLE_SCENE OFF CACHE BOOL "Scene loading is not RHI" FORCE)
    set(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION "${_pin_windowsSdk}")
    FetchContent_MakeAvailable(gfx)
    atelier_patch_gfx("${_gfx}")

    # Do not silently raise the engine's x64 CPU baseline to AVX2.
    get_target_property(_gfx_options gfx COMPILE_OPTIONS)
    list(FILTER _gfx_options EXCLUDE REGEX "^(/arch:AVX2|-march=x86-64-v3)$")
    set_property(TARGET gfx PROPERTY COMPILE_OPTIONS "${_gfx_options}")
    set_target_properties(gfx PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

    add_library(atelier_rhi STATIC "${PROJECT_SOURCE_DIR}/src/rhi/rhi.cpp")
    atelier_target(atelier_rhi)
    target_compile_features(atelier_rhi PUBLIC cxx_std_23)
    target_include_directories(atelier_rhi PUBLIC "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(atelier_rhi PUBLIC gfx)
    target_include_directories(atelier_rhi SYSTEM PUBLIC
        "${_gfx}/third_party/directx12-agility/build/native/include")

    add_executable(atelier_rhi_acceptance
        "${PROJECT_SOURCE_DIR}/acceptance/rhi/main.cpp"
        "${PROJECT_SOURCE_DIR}/acceptance/rhi/ray_query_cases.cpp"
        "${PROJECT_SOURCE_DIR}/acceptance/rhi/scene_lit_cases.cpp"
        "${PROJECT_SOURCE_DIR}/acceptance/rhi/resource_cases.cpp")
    atelier_target(atelier_rhi_acceptance)
    target_link_libraries(atelier_rhi_acceptance PRIVATE atelier_rhi)

    add_executable(atelier_gfx_regression "${PROJECT_SOURCE_DIR}/acceptance/rhi/upstream_regression.cpp")
    atelier_target(atelier_gfx_regression)
    target_link_libraries(atelier_gfx_regression PRIVATE gfx)
    target_include_directories(atelier_gfx_regression SYSTEM PRIVATE
        "${_gfx}/third_party/directx12-agility/build/native/include")

    atelier_deploy_rhi_runtime(atelier_rhi_acceptance)
    atelier_deploy_rhi_runtime(atelier_gfx_regression)

    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/licenses/rhi")
    configure_file("${_gfx}/LICENSE.md" "${CMAKE_BINARY_DIR}/licenses/rhi/LICENSE.gfx.md" COPYONLY)
    foreach(_name LICENSE-MS.txt LICENSE-MIT.txt LICENSE-LLVM.txt)
        configure_file("${_gfx}/third_party/directx-dxc/${_name}"
            "${CMAKE_BINARY_DIR}/licenses/rhi/DXC-${_name}" COPYONLY)
    endforeach()
    foreach(_name LICENSE.txt LICENSE-CODE.txt "distributable files.txt")
        configure_file("${_gfx}/third_party/directx12-agility/${_name}"
            "${CMAKE_BINARY_DIR}/licenses/rhi/Agility-${_name}" COPYONLY)
    endforeach()
    configure_file("${_gfx}/third_party/D3D12MemoryAllocator/LICENSE.txt"
        "${CMAKE_BINARY_DIR}/licenses/rhi/LICENSE.D3D12MA.txt" COPYONLY)
    foreach(_name license.txt ThirdPartyNotices.txt)
        configure_file("${_gfx}/third_party/WinPixEventRuntime/${_name}"
            "${CMAKE_BINARY_DIR}/licenses/rhi/PIX-${_name}" COPYONLY)
    endforeach()
    file(COPY "${_gfx}/third_party/AmdDxExt" DESTINATION "${CMAKE_BINARY_DIR}/licenses/rhi")
    configure_file("${PROJECT_SOURCE_DIR}/third_party/gfx/atelier.patch"
        "${CMAKE_BINARY_DIR}/licenses/rhi/atelier.patch" COPYONLY)

    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/run-rhi-acceptance.cmake"
        INPUT "${PROJECT_SOURCE_DIR}/cmake/run-rhi-acceptance.cmake.in")
    add_custom_target(rhi_acceptance
        COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_BINARY_DIR}/run-rhi-acceptance.cmake"
        DEPENDS atelier_rhi_acceptance atelier_gfx_regression
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}" USES_TERMINAL VERBATIM)
endfunction()

function(atelier_deploy_rhi_runtime target)
    set(_gfx "${CMAKE_BINARY_DIR}/_deps/gfx-src")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_gfx}/third_party/directx-dxc/bin/x64/dxcompiler.dll"
            "${_gfx}/third_party/directx-dxc/bin/x64/dxil.dll"
            "${_gfx}/third_party/directx12-agility/build/native/bin/x64/D3D12Core.dll"
            "${_gfx}/third_party/directx12-agility/build/native/bin/x64/d3d12SDKLayers.dll"
            "${_gfx}/third_party/WinPixEventRuntime/bin/x64/WinPixEventRuntime.dll"
            "$<TARGET_FILE_DIR:${target}>"
        VERBATIM)
endfunction()
