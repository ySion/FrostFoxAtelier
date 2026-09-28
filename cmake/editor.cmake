function(atelier_add_editor)
    add_library(atelier_editor_core STATIC
        "${PROJECT_SOURCE_DIR}/src/scene/scene.cpp"
        "${PROJECT_SOURCE_DIR}/src/editor/session.cpp"
        "${PROJECT_SOURCE_DIR}/src/editor/camera.cpp")
    atelier_target(atelier_editor_core)
    target_include_directories(atelier_editor_core PUBLIC "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(atelier_editor_core PUBLIC atelier_runtime)
    add_library(atelier_editor_application STATIC
        "${PROJECT_SOURCE_DIR}/src/application/window.cpp"
        "${PROJECT_SOURCE_DIR}/src/application/application.cpp"
        "${PROJECT_SOURCE_DIR}/src/editor/workspace.cpp")
    atelier_target(atelier_editor_application)
    target_compile_definitions(atelier_editor_application PUBLIC UNICODE _UNICODE)
    target_link_libraries(atelier_editor_application PUBLIC atelier_editor_core atelier_ui_layout atelier_ui_protocol atelier_rhi)
    add_executable(atelier_editor
        "${PROJECT_SOURCE_DIR}/src/application/main.cpp"
        "${PROJECT_SOURCE_DIR}/acceptance/editor/window.cpp")
    atelier_target(atelier_editor)
    target_link_libraries(atelier_editor PRIVATE atelier_editor_application atelier_ui atelier_renderer user32 ole32 windowscodecs)
    atelier_deploy_rhi_runtime(atelier_editor)
    add_executable(atelier_editor_scene_acceptance "${PROJECT_SOURCE_DIR}/acceptance/editor/scene.cpp")
    atelier_target(atelier_editor_scene_acceptance)
    target_link_libraries(atelier_editor_scene_acceptance PRIVATE atelier_editor_core)
    add_custom_target(editor_scene_acceptance
        COMMAND $<TARGET_FILE:atelier_editor_scene_acceptance>
        DEPENDS atelier_editor_scene_acceptance USES_TERMINAL VERBATIM)
    add_custom_target(editor_acceptance
        COMMAND $<TARGET_FILE:atelier_editor_scene_acceptance>
        COMMAND $<TARGET_FILE:atelier_editor> --acceptance "${CMAKE_BINARY_DIR}/editor-evidence"
        DEPENDS atelier_editor atelier_editor_scene_acceptance USES_TERMINAL VERBATIM)
endfunction()
