function(atelier_add_editor)
    add_library(atelier_editor_core STATIC
        "${PROJECT_SOURCE_DIR}/src/scene/scene.cpp"
        "${PROJECT_SOURCE_DIR}/src/editor/session.cpp"
        "${PROJECT_SOURCE_DIR}/src/editor/camera.cpp")
    atelier_target(atelier_editor_core)
    target_include_directories(atelier_editor_core PUBLIC "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(atelier_editor_core PUBLIC atelier_runtime)
    add_executable(atelier_editor_scene_acceptance "${PROJECT_SOURCE_DIR}/acceptance/editor/scene.cpp")
    atelier_target(atelier_editor_scene_acceptance)
    target_link_libraries(atelier_editor_scene_acceptance PRIVATE atelier_editor_core)
    add_custom_target(editor_scene_acceptance
        COMMAND $<TARGET_FILE:atelier_editor_scene_acceptance>
        DEPENDS atelier_editor_scene_acceptance USES_TERMINAL VERBATIM)
endfunction()
