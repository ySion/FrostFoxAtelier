include_guard(GLOBAL)

function(atelier_add_renderer)
    foreach(_shader raster_position material_common scene_common scene_vs scene_ps ui_common ui_vs ui_ps viewport_vs background_ps grid_ps cavity_ps)
        set(_path "${PROJECT_SOURCE_DIR}/src/renderer/shaders/${_shader}.hlsl")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_path}")
        file(READ "${_path}" ${_shader})
    endforeach()
    foreach(_shader scene_vs scene_ps)
        set(${_shader} "${raster_position}\n${material_common}\n${scene_common}\n${${_shader}}")
    endforeach()
    set(viewport_vs "${raster_position}\n${viewport_vs}")
    foreach(_shader ui_vs ui_ps)
        set(${_shader} "${ui_common}\n${${_shader}}")
    endforeach()
    foreach(_shader lit_common lit_light_sampling lit_restir lit_clustered lit_gbuffer lit_shadow lit_shadow_sampling lit_direct lit_ddgi_common lit_ddgi lit_ddgi_debug lit_post lit_smaa lit_ssil)
        set(_path "${PROJECT_SOURCE_DIR}/src/renderer/shaders/${_shader}.hlsl")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_path}")
        file(READ "${_path}" ${_shader})
    endforeach()
    set(_smaa_reference_path "${PROJECT_SOURCE_DIR}/third_party/smaa/SMAA.hlsl")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_smaa_reference_path}")
    file(READ "${_smaa_reference_path}" _smaa_reference)
    # The pinned reference has two Latin-1 middle dots in its ASCII flowchart.
    # Normalize only the embedded copy; keep vendored bytes and license intact.
    string(ASCII 183 _smaa_legacy_middle_dot)
    string(REPLACE "${_smaa_legacy_middle_dot}" "." _smaa_reference "${_smaa_reference}")
    string(REPLACE "// SMAA_REFERENCE_SOURCE" "${_smaa_reference}" lit_smaa "${lit_smaa}")
    set(lit_restir "${lit_direct}\n${lit_restir}")
    set(lit_clustered "${lit_direct}\n${lit_shadow_sampling}\n${lit_clustered}")
    set(lit_shadow "${lit_gbuffer}\n${lit_shadow}")
    foreach(_shader lit_direct lit_restir lit_clustered lit_ddgi)
        set(${_shader} "${lit_light_sampling}\n${${_shader}}")
    endforeach()
    foreach(_shader lit_ddgi lit_ddgi_debug)
        set(${_shader} "${lit_ddgi_common}\n${${_shader}}")
    endforeach()
    foreach(_shader lit_gbuffer lit_shadow lit_direct lit_restir lit_clustered lit_ddgi lit_ddgi_debug lit_post lit_smaa lit_ssil)
        set(${_shader} "${raster_position}\n${material_common}\n${lit_common}\n${${_shader}}")
    endforeach()
    configure_file("${PROJECT_SOURCE_DIR}/src/renderer/shader_source.hpp.in"
        "${CMAKE_CURRENT_BINARY_DIR}/renderer-generated/shader_source.hpp" @ONLY)
    add_library(atelier_renderer STATIC "${PROJECT_SOURCE_DIR}/src/renderer/renderer.cpp"
        "${PROJECT_SOURCE_DIR}/src/renderer/scene_lit.cpp"
        "${PROJECT_SOURCE_DIR}/src/renderer/shadow_cache.cpp"
        "${PROJECT_SOURCE_DIR}/src/renderer/light_frame.cpp")
    atelier_target(atelier_renderer)
    target_include_directories(atelier_renderer PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/renderer-generated")
    target_link_libraries(atelier_renderer PUBLIC atelier_rhi atelier_ui_protocol atelier_geometry atelier_foundation)
    add_executable(atelier_renderer_acceptance "${PROJECT_SOURCE_DIR}/acceptance/renderer/main.cpp")
    atelier_target(atelier_renderer_acceptance)
    target_link_libraries(atelier_renderer_acceptance PRIVATE atelier_renderer)
    atelier_deploy_rhi_runtime(atelier_renderer_acceptance)
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/run-renderer-acceptance.cmake"
        INPUT "${PROJECT_SOURCE_DIR}/cmake/run-renderer-acceptance.cmake.in")
    # One window target per entry, so a targeted run (and its GPU validation level) can pick entries.
    set(_entries)
    foreach(_mode default scene-lit scene-lit-benchmark scene-lit-temporal scene-lit-quality scene-lit-stability
            scene-lit-restir-suite scene-lit-clustered scene-lit-shadows scene-lit-cost)
        string(REPLACE "-" "_" _entry "renderer_acceptance_${_mode}")
        add_custom_target(${_entry}
            COMMAND "${CMAKE_COMMAND}" -D "MODE=${_mode}" -P "${CMAKE_BINARY_DIR}/run-renderer-acceptance.cmake"
            DEPENDS atelier_renderer_acceptance
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}" USES_TERMINAL VERBATIM)
        list(APPEND _entries ${_entry})
    endforeach()
    add_custom_target(renderer_acceptance DEPENDS ${_entries})
    set_property(GLOBAL PROPERTY ATELIER_RENDERER_ACCEPTANCE ${_entries})
endfunction()
