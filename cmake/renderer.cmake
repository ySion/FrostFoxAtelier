include_guard(GLOBAL)

function(atelier_add_renderer)
    foreach(_shader scene_common scene_vs scene_ps ui_common ui_vs solid_ps coverage_ps image_ps viewport_vs background_ps grid_ps)
        set(_path "${PROJECT_SOURCE_DIR}/src/renderer/shaders/${_shader}.hlsl")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_path}")
        file(READ "${_path}" ${_shader})
    endforeach()
    foreach(_shader scene_vs scene_ps)
        set(${_shader} "${scene_common}\n${${_shader}}")
    endforeach()
    foreach(_shader ui_vs solid_ps coverage_ps image_ps)
        set(${_shader} "${ui_common}\n${${_shader}}")
    endforeach()
    configure_file("${PROJECT_SOURCE_DIR}/src/renderer/shader_source.hpp.in"
        "${CMAKE_CURRENT_BINARY_DIR}/renderer-generated/shader_source.hpp" @ONLY)
    add_library(atelier_renderer STATIC "${PROJECT_SOURCE_DIR}/src/renderer/renderer.cpp")
    atelier_target(atelier_renderer)
    target_include_directories(atelier_renderer PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/renderer-generated")
    target_link_libraries(atelier_renderer PUBLIC atelier_rhi atelier_ui_protocol)
    add_executable(atelier_renderer_acceptance "${PROJECT_SOURCE_DIR}/acceptance/renderer/main.cpp")
    atelier_target(atelier_renderer_acceptance)
    target_link_libraries(atelier_renderer_acceptance PRIVATE atelier_renderer)
    atelier_deploy_rhi_runtime(atelier_renderer_acceptance)
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/run-renderer-acceptance.cmake"
        INPUT "${PROJECT_SOURCE_DIR}/cmake/run-renderer-acceptance.cmake.in")
    add_custom_target(renderer_acceptance
        COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_BINARY_DIR}/run-renderer-acceptance.cmake"
        DEPENDS atelier_renderer_acceptance
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}" USES_TERMINAL VERBATIM)
endfunction()
