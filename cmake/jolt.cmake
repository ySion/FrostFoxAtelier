function(atelier_add_jolt)
    set(_jolt_source "${PROJECT_SOURCE_DIR}/third_party/jolt")
    if(NOT EXISTS "${_jolt_source}/Build/CMakeLists.txt")
        message(FATAL_ERROR
            "Jolt source is missing. Run: git submodule update --init --recursive third_party/jolt")
    endif()

    # Keep upstream options local to this dependency. CMP0077 makes its option()
    # calls honor these values without polluting other libraries' cache options.
    set(JPH_BUILD_SHARED_LIBS OFF)
    set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF)
    set(OVERRIDE_CXX_FLAGS OFF)
    set(INTERPROCEDURAL_OPTIMIZATION OFF)
    set(ENABLE_ALL_WARNINGS OFF)
    set(ENABLE_INSTALL OFF)
    set(DOUBLE_PRECISION OFF)
    set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF)
    set(DEBUG_RENDERER_IN_DISTRIBUTION OFF)
    set(PROFILER_IN_DEBUG_AND_RELEASE OFF)
    set(PROFILER_IN_DISTRIBUTION OFF)
    set(JPH_USE_EXTERNAL_PROFILE OFF)
    set(ENABLE_OBJECT_STREAM OFF)
    set(JPH_USE_DX12 OFF)
    set(JPH_USE_VK OFF)
    set(JPH_USE_MTL OFF)
    set(JPH_USE_CPU_COMPUTE OFF)

    # Windows x64 is the current minimum; do not require AVX2-capable clients.
    foreach(_feature SSE4_1 SSE4_2 AVX AVX2 AVX512 LZCNT TZCNT F16C FMADD)
        set(USE_${_feature} OFF)
    endforeach()

    add_subdirectory("${_jolt_source}/Build" "${CMAKE_BINARY_DIR}/third_party/jolt"
        EXCLUDE_FROM_ALL SYSTEM)
    set_target_properties(Jolt PROPERTIES
        CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    configure_file("${_jolt_source}/LICENSE"
        "${CMAKE_BINARY_DIR}/LICENSE.jolt.txt" COPYONLY)

    add_library(atelier_physics STATIC
        "${PROJECT_SOURCE_DIR}/modules/engine/physics/src/components.cpp"
        "${PROJECT_SOURCE_DIR}/modules/engine/physics/src/physics.cpp")
    atelier_target(atelier_physics)
    target_include_directories(atelier_physics PUBLIC "${PROJECT_SOURCE_DIR}/modules/engine/physics/include")
    target_link_libraries(atelier_physics PUBLIC atelier_world atelier_geometry PRIVATE Jolt::Jolt)

    add_executable(atelier_physics_acceptance "${PROJECT_SOURCE_DIR}/acceptance/physics/main.cpp")
    atelier_target(atelier_physics_acceptance)
    target_include_directories(atelier_physics_acceptance PRIVATE "${PROJECT_SOURCE_DIR}/modules/engine/world/internal")
    target_link_libraries(atelier_physics_acceptance PRIVATE atelier_physics atelier_presentation)
    atelier_headless_acceptance(physics_acceptance
        COMMAND $<TARGET_FILE:atelier_physics_acceptance>
        DEPENDS atelier_physics_acceptance)
endfunction()
