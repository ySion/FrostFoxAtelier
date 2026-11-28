include_guard(GLOBAL)

function(atelier_add_renderer)
    foreach(_shader
        common_bindless
        raster_position
        visbuffer_cull_cs
        visbuffer_mesh
        visbuffer_masked_ps
        vsm_cull_cs
        vsm_mesh
        vis_unpack_classify_cs
        material_shade_bin_cs
        shadow_ray_query_cs
        direct_light_cs
        secondary_gi_hit_cs
        gi_four_layer_cs
        reflections_atmosphere_cs
        post_taau_unjitter_cs
        wireframe_mesh
        ui_common
        ui_vs
        ui_ps
        viewport_vs
        background_ps
        grid_ps
        cavity_ps)
        set(_path "${PROJECT_SOURCE_DIR}/modules/render/renderer/shaders/${_shader}.hlsl")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_path}")
        file(READ "${_path}" ${_shader})
    endforeach()

    foreach(_shader
        visbuffer_cull_cs
        visbuffer_masked_ps
        vsm_cull_cs
        vis_unpack_classify_cs
        material_shade_bin_cs
        shadow_ray_query_cs
        direct_light_cs
        secondary_gi_hit_cs
        gi_four_layer_cs
        reflections_atmosphere_cs
        post_taau_unjitter_cs)
        set(${_shader} "${common_bindless}\n${${_shader}}")
    endforeach()

    foreach(_shader visbuffer_mesh vsm_mesh wireframe_mesh)
        set(${_shader} "${common_bindless}\n${raster_position}\n${${_shader}}")
    endforeach()

    set(viewport_vs "${raster_position}\n${viewport_vs}")
    foreach(_shader ui_vs ui_ps)
        set(${_shader} "${ui_common}\n${${_shader}}")
    endforeach()

    configure_file("${PROJECT_SOURCE_DIR}/modules/render/renderer/src/detail/shader_source.hpp.in"
        "${CMAKE_CURRENT_BINARY_DIR}/renderer-generated/shader_source.hpp" @ONLY)

    add_library(atelier_renderer STATIC
        "${PROJECT_SOURCE_DIR}/modules/render/renderer/src/renderer.cpp"
        "${PROJECT_SOURCE_DIR}/modules/render/renderer/src/detail/geometry_pool.cpp"
        "${PROJECT_SOURCE_DIR}/modules/render/renderer/src/detail/render_scene.cpp"
        "${PROJECT_SOURCE_DIR}/modules/render/renderer/src/detail/vsm_shadow_cache.cpp"
        "${PROJECT_SOURCE_DIR}/modules/render/renderer/src/detail/lighting_lut.cpp"
        "${PROJECT_SOURCE_DIR}/modules/render/renderer/src/detail/material_archetypes.cpp")
    atelier_target(atelier_renderer)
    target_include_directories(atelier_renderer
        PUBLIC "${PROJECT_SOURCE_DIR}/modules/render/renderer/include"
        PRIVATE "${PROJECT_SOURCE_DIR}/modules/render/renderer/internal"
                "${PROJECT_SOURCE_DIR}/third_party"
                "${CMAKE_CURRENT_BINARY_DIR}/renderer-generated")
    target_link_libraries(atelier_renderer
        PUBLIC
            atelier_rhi
            atelier_rdg
            atelier_ui_protocol
            atelier_geometry
            atelier_foundation
            atelier_rendering
            atelier_material
        PRIVATE
            atelier_assets
            atelier_rdg_internal
            atelier_logging)

    add_executable(atelier_renderer_acceptance "${PROJECT_SOURCE_DIR}/acceptance/renderer/main.cpp")
    atelier_target(atelier_renderer_acceptance)
    target_link_libraries(atelier_renderer_acceptance
        PRIVATE atelier_renderer atelier_source atelier_material atelier_assets)
    atelier_deploy_rhi_runtime(atelier_renderer_acceptance)

    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/run-renderer-acceptance.cmake"
        INPUT "${PROJECT_SOURCE_DIR}/cmake/run-renderer-acceptance.cmake.in")
    set(_entries)
    foreach(_mode default scene-lit scene-lit-benchmark material-programs)
        string(REPLACE "-" "_" _entry "renderer_acceptance_${_mode}")
        add_custom_target(${_entry}
            COMMAND "${CMAKE_COMMAND}" -D "MODE=${_mode}" -P "${CMAKE_BINARY_DIR}/run-renderer-acceptance.cmake"
            DEPENDS atelier_renderer_acceptance
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}" USES_TERMINAL VERBATIM)
        if(NOT _mode STREQUAL "scene-lit-benchmark")
            list(APPEND _entries ${_entry})
        endif()
    endforeach()
    add_custom_target(renderer_acceptance DEPENDS ${_entries})
    set_property(GLOBAL PROPERTY ATELIER_RENDERER_ACCEPTANCE ${_entries})
endfunction()
