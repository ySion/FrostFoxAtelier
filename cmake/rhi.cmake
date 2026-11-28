include(FetchContent)

function(atelier_rhi_dependency name key source_dir)
    file(READ "${PROJECT_SOURCE_DIR}/cmake/dependencies-lock.json" _lock)
    foreach(_field url sha256 filename)
        string(JSON _${_field} GET "${_lock}" "${key}" "${_field}")
    endforeach()
    FetchContent_Declare(${name}
        URL "${_url}" URL_HASH "SHA256=${_sha256}"
        DOWNLOAD_NAME "${_filename}"
        DOWNLOAD_DIR "${PROJECT_SOURCE_DIR}/.tools/downloads"
        SOURCE_DIR "${source_dir}"
        SOURCE_SUBDIR "atelier_ignore_upstream_cmakelists"
        EXCLUDE_FROM_ALL SYSTEM)
endfunction()

function(atelier_patch_nri source_dir)
    find_package(Git REQUIRED)
    set(_patch "${PROJECT_SOURCE_DIR}/third_party/nri/atelier.patch")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_patch}")
    file(SHA256 "${_patch}" _patch_sha)
    set(_stamp "${source_dir}/.atelier-patch-sha256")
    if(EXISTS "${_stamp}")
        file(READ "${_stamp}" _applied_sha)
        if(_applied_sha STREQUAL _patch_sha)
            return()
        endif()
    endif()
    file(REMOVE "${_stamp}")
    set(_patched_files
        Include/Extensions/NRIDeviceCreation.h
        Source/D3D12/CommandBufferD3D12.hpp
        Source/D3D12/DeviceD3D12.hpp
        Source/D3D12/FenceD3D12.h
        Source/D3D12/FenceD3D12.hpp
        Source/D3D12/QueueD3D12.hpp)
    foreach(_file ${_patched_files})
        get_filename_component(_backup_dir "${source_dir}/.atelier-upstream/${_file}" DIRECTORY)
        file(MAKE_DIRECTORY "${_backup_dir}")
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
        message(FATAL_ERROR "Pinned NRI patch does not apply: ${_out}${_err}")
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply "${_patch}"
        WORKING_DIRECTORY "${source_dir}" COMMAND_ERROR_IS_FATAL ANY)
    file(WRITE "${_stamp}" "${_patch_sha}")
endfunction()

function(atelier_add_rhi)
    file(READ "${PROJECT_SOURCE_DIR}/cmake/dependencies-lock.json" _rhi_lock)
    string(JSON _nri_commit GET "${_rhi_lock}" nri commit)
    string(SUBSTRING "${_nri_commit}" 0 8 _nri_short)
    set(_deps_root "${CMAKE_BINARY_DIR}/_deps/rhi-nri-${_nri_short}")
    set(_nri "${_deps_root}/nri")
    set(_dxc "${_deps_root}/directx-dxc")
    set(_agility "${_deps_root}/directx12-agility")
    set(_d3d12ma "${_deps_root}/D3D12MemoryAllocator")
    set(_pix "${_deps_root}/WinPixEventRuntime")

    atelier_rhi_dependency(atelier_dep_nri nri "${_nri}")
    atelier_rhi_dependency(atelier_dep_dxc dxc "${_dxc}")
    atelier_rhi_dependency(atelier_dep_agility agility "${_agility}")
    atelier_rhi_dependency(atelier_dep_d3d12ma d3d12ma "${_d3d12ma}")
    atelier_rhi_dependency(atelier_dep_pix pix "${_pix}")
    FetchContent_MakeAvailable(
        atelier_dep_nri atelier_dep_dxc atelier_dep_agility atelier_dep_d3d12ma atelier_dep_pix)
    atelier_patch_nri("${_nri}")

    set_property(GLOBAL PROPERTY ATELIER_RHI_DXC_DIR "${_dxc}")
    set_property(GLOBAL PROPERTY ATELIER_RHI_AGILITY_DIR "${_agility}")
    set_property(GLOBAL PROPERTY ATELIER_RHI_PIX_DIR "${_pix}")

    add_library(nri STATIC
        "${_nri}/Source/Shared/Shared.cpp"
        "${_nri}/Source/D3D12/ImplD3D12.cpp"
        "${_nri}/Source/Validation/ImplVal.cpp"
        "${_nri}/Source/Creation/Creation.cpp")
    set_target_properties(nri PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    target_include_directories(nri SYSTEM PUBLIC "${_nri}/Include"
        PRIVATE "${_nri}/Source/Shared" "${_agility}/build/native/include"
                "${_d3d12ma}/include" "${_d3d12ma}/src")
    target_compile_definitions(nri
        PUBLIC
            NRI_STATIC_LIBRARY=1
        PRIVATE
            WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
            _HAS_EXCEPTIONS=0
            NRI_ENABLE_D3D12_SUPPORT=1
            NRI_ENABLE_VALIDATION_SUPPORT=1
            NRI_ENABLE_DEBUG_NAMES_AND_ANNOTATIONS=1
            NRI_ENABLE_AGILITY_SDK_SUPPORT=1
            NRI_AGILITY_SDK_VERSION_MAJOR=620)
    target_compile_options(nri PRIVATE /W3 /utf-8 /EHs-c- /wd4324
        -Wno-missing-field-initializers -Wno-nullability-completeness -Wno-unused-parameter)
    target_link_libraries(nri PUBLIC d3d12 dxgi dxguid)

    add_library(atelier_rhi_internal INTERFACE)
    target_include_directories(atelier_rhi_internal INTERFACE
        "${PROJECT_SOURCE_DIR}/modules/render/rhi/internal")
    target_include_directories(atelier_rhi_internal SYSTEM INTERFACE
        "${_agility}/build/native/include")
    target_link_libraries(atelier_rhi_internal INTERFACE nri)

    add_library(atelier_rhi STATIC
        "${PROJECT_SOURCE_DIR}/modules/render/rhi/src/rhi.cpp"
        "${PROJECT_SOURCE_DIR}/modules/render/rhi/src/shader_compiler.cpp")
    atelier_target(atelier_rhi)
    target_compile_features(atelier_rhi PUBLIC cxx_std_23)
    target_include_directories(atelier_rhi PUBLIC
        "${PROJECT_SOURCE_DIR}/modules/render/rhi/include")
    target_include_directories(atelier_rhi SYSTEM PRIVATE "${_dxc}/inc")
    target_link_libraries(atelier_rhi
        PUBLIC
            atelier_foundation
        PRIVATE
            atelier_rhi_internal
            atelier_platform
            "${_dxc}/lib/x64/dxcompiler.lib"
            ole32
            user32)

    add_library(atelier_rdg_internal INTERFACE)
    target_include_directories(atelier_rdg_internal INTERFACE
        "${PROJECT_SOURCE_DIR}/modules/render/rdg/internal")
    target_link_libraries(atelier_rdg_internal INTERFACE atelier_rhi_internal)

    add_library(atelier_rdg STATIC
        "${PROJECT_SOURCE_DIR}/modules/render/rdg/src/rdg.cpp")
    atelier_target(atelier_rdg)
    target_compile_features(atelier_rdg PUBLIC cxx_std_23)
    target_include_directories(atelier_rdg
        PUBLIC "${PROJECT_SOURCE_DIR}/modules/render/rdg/include")
    target_link_libraries(atelier_rdg
        PUBLIC atelier_rhi atelier_foundation
        PRIVATE atelier_rdg_internal)

    add_executable(atelier_rhi_acceptance "${PROJECT_SOURCE_DIR}/acceptance/rhi/main.cpp")
    atelier_target(atelier_rhi_acceptance)
    target_link_libraries(atelier_rhi_acceptance PRIVATE atelier_rhi atelier_rdg atelier_rdg_internal)
    atelier_deploy_rhi_runtime(atelier_rhi_acceptance)

    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/licenses/rhi")
    configure_file("${_nri}/LICENSE.txt" "${CMAKE_BINARY_DIR}/licenses/rhi/LICENSE.nri.txt" COPYONLY)
    foreach(_name LICENSE-MS.txt LICENSE-MIT.txt LICENSE-LLVM.txt)
        configure_file("${_dxc}/${_name}" "${CMAKE_BINARY_DIR}/licenses/rhi/DXC-${_name}" COPYONLY)
    endforeach()
    foreach(_name LICENSE.txt LICENSE-CODE.txt "distributable files.txt")
        configure_file("${_agility}/${_name}" "${CMAKE_BINARY_DIR}/licenses/rhi/Agility-${_name}" COPYONLY)
    endforeach()
    configure_file("${_d3d12ma}/LICENSE.txt" "${CMAKE_BINARY_DIR}/licenses/rhi/LICENSE.D3D12MA.txt" COPYONLY)
    foreach(_name license.txt ThirdPartyNotices.txt)
        configure_file("${_pix}/${_name}" "${CMAKE_BINARY_DIR}/licenses/rhi/PIX-${_name}" COPYONLY)
    endforeach()
    configure_file("${PROJECT_SOURCE_DIR}/third_party/nri/atelier.patch"
        "${CMAKE_BINARY_DIR}/licenses/rhi/nri-atelier.patch" COPYONLY)

    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/run-rhi-acceptance.cmake"
        INPUT "${PROJECT_SOURCE_DIR}/cmake/run-rhi-acceptance.cmake.in")
    add_custom_target(rhi_acceptance
        COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_BINARY_DIR}/run-rhi-acceptance.cmake"
        DEPENDS atelier_rhi_acceptance
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}" USES_TERMINAL VERBATIM)
endfunction()

function(atelier_deploy_rhi_runtime target)
    get_property(_dxc GLOBAL PROPERTY ATELIER_RHI_DXC_DIR)
    get_property(_agility GLOBAL PROPERTY ATELIER_RHI_AGILITY_DIR)
    get_property(_pix GLOBAL PROPERTY ATELIER_RHI_PIX_DIR)
    target_sources(${target} PRIVATE "${PROJECT_SOURCE_DIR}/modules/render/rhi/src/agility_export.cpp")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_dxc}/bin/x64/dxcompiler.dll"
            "${_dxc}/bin/x64/dxil.dll"
            "${_agility}/build/native/bin/x64/D3D12Core.dll"
            "${_agility}/build/native/bin/x64/d3d12SDKLayers.dll"
            "${_pix}/bin/x64/WinPixEventRuntime.dll"
            "$<TARGET_FILE_DIR:${target}>"
        VERBATIM)
endfunction()
