include(${CMAKE_CURRENT_LIST_DIR}/json.cmake)

function(atelier_add_editor)
    add_library(atelier_control_transport STATIC "${PROJECT_SOURCE_DIR}/src/control/control.cpp")
    atelier_target(atelier_control_transport)
    target_include_directories(atelier_control_transport PUBLIC "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(atelier_control_transport PUBLIC nlohmann_json::nlohmann_json PRIVATE advapi32 bcrypt ole32)
    add_executable(atelier_control "${PROJECT_SOURCE_DIR}/src/control/main.cpp")
    atelier_target(atelier_control)
    target_link_libraries(atelier_control PRIVATE atelier_control_transport)
    # Official scenes are shipped alongside the executable, independently of cwd/source checkout.
    set(example_root "${PROJECT_SOURCE_DIR}/assets/examples")
    file(GLOB_RECURSE example_files CONFIGURE_DEPENDS RELATIVE "${example_root}"
        "${example_root}/Config/*" "${example_root}/Content/*" "${example_root}/Source/*" "${example_root}/Examples/*")
    foreach(example_file IN LISTS example_files)
        configure_file("${example_root}/${example_file}" "${CMAKE_BINARY_DIR}/examples/${example_file}" COPYONLY)
    endforeach()
    find_package(Git QUIET)
    set(build_info "${CMAKE_BINARY_DIR}/generated/atelier/build_info.hpp")
    add_custom_target(atelier_editor_build_info
        COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}" "-DOUTPUT=${build_info}"
            "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}" "-DCONFIGURATION=$<CONFIG>"
            -P "${PROJECT_SOURCE_DIR}/cmake/editor-build-info.cmake"
        BYPRODUCTS "${build_info}" VERBATIM)
    configure_file("${PROJECT_SOURCE_DIR}/assets/editor.ss" "${CMAKE_BINARY_DIR}/editor.ss" COPYONLY)
    configure_file("${PROJECT_SOURCE_DIR}/assets/editor.ui.source" "${CMAKE_BINARY_DIR}/editor.ui.source" COPYONLY)
    foreach(font_asset SarasaUiSC-Regular.ttf SarasaUiSC-SemiBold.ttf
            SourceCodePro-Regular.otf SourceCodePro-Bold.otf sarasa-LICENSE.txt source-code-pro-LICENSE.md sources.json)
        configure_file("${PROJECT_SOURCE_DIR}/assets/fonts/${font_asset}" "${CMAKE_BINARY_DIR}/fonts/${font_asset}" COPYONLY)
    endforeach()
    add_library(atelier_editor_core STATIC
        "${PROJECT_SOURCE_DIR}/src/editor/examples.cpp"
        "${PROJECT_SOURCE_DIR}/src/editor/assets.cpp"
        "${PROJECT_SOURCE_DIR}/src/editor/project.cpp"
        "${PROJECT_SOURCE_DIR}/src/editor/project_materials.cpp"
        "${PROJECT_SOURCE_DIR}/src/editor/image_decode.cpp"
        "${PROJECT_SOURCE_DIR}/src/editor/obj_import.cpp"
        "${PROJECT_SOURCE_DIR}/src/editor/project_code.cpp"
        "${PROJECT_SOURCE_DIR}/src/editor/view_options.cpp"
        "${PROJECT_SOURCE_DIR}/src/editor/scene.cpp"
        "${PROJECT_SOURCE_DIR}/src/editor/session.cpp"
        "${PROJECT_SOURCE_DIR}/src/editor/session_materials.cpp"
        "${PROJECT_SOURCE_DIR}/src/editor/camera.cpp"
        "${PROJECT_SOURCE_DIR}/src/editor/viewport_tool.cpp")
    atelier_target(atelier_editor_core)
    target_include_directories(atelier_editor_core PUBLIC "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(atelier_editor_core PUBLIC atelier_world atelier_ui_language atelier_physics PRIVATE ole32 shell32 windowscodecs nlohmann_json::nlohmann_json)
    add_library(atelier_editor_application STATIC
        "${PROJECT_SOURCE_DIR}/src/application/window.cpp"
        "${PROJECT_SOURCE_DIR}/src/application/input.cpp"
        "${PROJECT_SOURCE_DIR}/src/application/viewport.cpp"
        "${PROJECT_SOURCE_DIR}/src/application/presentation.cpp"
        "${PROJECT_SOURCE_DIR}/src/application/play_process.cpp"
        "${PROJECT_SOURCE_DIR}/src/application/project_runner.cpp"
        "${PROJECT_SOURCE_DIR}/src/application/popup.cpp"
        "${PROJECT_SOURCE_DIR}/src/application/application.cpp"
        "${PROJECT_SOURCE_DIR}/src/application/recovery.cpp"
        "${PROJECT_SOURCE_DIR}/src/application/editor_control.cpp"
        "${PROJECT_SOURCE_DIR}/src/application/control_artifacts.cpp"
        "${PROJECT_SOURCE_DIR}/src/editor/workspace.cpp"
        "${PROJECT_SOURCE_DIR}/src/editor/workspace_materials.cpp"
        "${PROJECT_SOURCE_DIR}/src/editor/world_panel.cpp"
        "${PROJECT_SOURCE_DIR}/src/editor/performance.cpp"
        "${PROJECT_SOURCE_DIR}/src/editor/asset_browser.cpp"
        "${PROJECT_SOURCE_DIR}/src/editor/source_editor.cpp"
        "${PROJECT_SOURCE_DIR}/src/editor/project_ui.cpp"
        "${PROJECT_SOURCE_DIR}/src/application/project.cpp"
        "${PROJECT_SOURCE_DIR}/src/application/project_execution.cpp"
        "${PROJECT_SOURCE_DIR}/src/editor/control_workbench.cpp")
    atelier_target(atelier_editor_application)
    add_dependencies(atelier_editor_application atelier_editor_build_info)
    target_include_directories(atelier_editor_application PUBLIC "${CMAKE_BINARY_DIR}/generated")
    target_compile_definitions(atelier_editor_application PUBLIC UNICODE _UNICODE)
    target_compile_definitions(atelier_editor_application PRIVATE "ATELIER_UI_AUTHORING_PATH=L\"${PROJECT_SOURCE_DIR}/assets/editor.ui.source\"")
    target_link_libraries(atelier_editor_application PUBLIC atelier_editor_core atelier_ui atelier_rhi atelier_control_transport)
    add_executable(atelier_editor
        "${PROJECT_SOURCE_DIR}/src/application/editor.manifest"
        "${PROJECT_SOURCE_DIR}/src/application/main.cpp"
        "${PROJECT_SOURCE_DIR}/acceptance/editor/window.cpp")
    atelier_target(atelier_editor)
    target_link_libraries(atelier_editor PRIVATE atelier_editor_application atelier_ui atelier_renderer nlohmann_json::nlohmann_json user32 ole32 shell32 windowscodecs imm32 uuid gdi32)
    atelier_deploy_rhi_runtime(atelier_editor)
    add_executable(atelier_editor_control_acceptance "${PROJECT_SOURCE_DIR}/acceptance/editor/control.cpp")
    atelier_target(atelier_editor_control_acceptance)
    target_link_libraries(atelier_editor_control_acceptance PRIVATE atelier_control_transport user32)
    add_custom_target(editor_control_acceptance
        COMMAND $<TARGET_FILE:atelier_editor_control_acceptance> $<TARGET_FILE:atelier_editor> "${CMAKE_BINARY_DIR}/control-acceptance"
        DEPENDS atelier_editor_control_acceptance atelier_editor atelier_control USES_TERMINAL VERBATIM)
    add_custom_target(editor_control_path_acceptance
        COMMAND $<TARGET_FILE:atelier_editor_control_acceptance> $<TARGET_FILE:atelier_editor> "${CMAKE_BINARY_DIR}/control-path-acceptance" --repro-paths
        DEPENDS atelier_editor_control_acceptance atelier_editor atelier_control USES_TERMINAL VERBATIM)
    add_executable(atelier_editor_document_acceptance
        "${PROJECT_SOURCE_DIR}/src/application/editor.manifest"
        "${PROJECT_SOURCE_DIR}/acceptance/editor/document.cpp")
    atelier_target(atelier_editor_document_acceptance)
    target_link_libraries(atelier_editor_document_acceptance PRIVATE atelier_editor_application nlohmann_json::nlohmann_json)
    add_custom_target(editor_document_acceptance
        COMMAND $<TARGET_FILE:atelier_editor_document_acceptance>
        DEPENDS atelier_editor_document_acceptance USES_TERMINAL VERBATIM)
    add_custom_target(editor_recovery_acceptance
        COMMAND $<TARGET_FILE:atelier_editor_document_acceptance> --recovery
        DEPENDS atelier_editor_document_acceptance USES_TERMINAL VERBATIM)
    add_custom_target(editor_material_acceptance
        COMMAND $<TARGET_FILE:atelier_editor_document_acceptance> --materials
        DEPENDS atelier_editor_document_acceptance USES_TERMINAL VERBATIM)
    add_executable(atelier_project_scene_acceptance "${PROJECT_SOURCE_DIR}/acceptance/editor/project_scene.cpp")
    atelier_target(atelier_project_scene_acceptance)
    target_link_libraries(atelier_project_scene_acceptance PRIVATE atelier_editor_core atelier_source atelier_physics nlohmann_json::nlohmann_json)
    add_custom_target(project_scene_acceptance
        COMMAND $<TARGET_FILE:atelier_project_scene_acceptance>
        DEPENDS atelier_project_scene_acceptance USES_TERMINAL VERBATIM)
    add_executable(atelier_project_integration_acceptance "${PROJECT_SOURCE_DIR}/acceptance/editor/project_execution.cpp")
    atelier_target(atelier_project_integration_acceptance)
    target_link_libraries(atelier_project_integration_acceptance PRIVATE atelier_editor_application)
    add_dependencies(atelier_project_integration_acceptance atelier_editor)
    add_custom_target(project_integration_acceptance
        COMMAND $<TARGET_FILE:atelier_project_integration_acceptance>
        DEPENDS atelier_project_integration_acceptance USES_TERMINAL VERBATIM)
    add_custom_target(editor_physics_project_acceptance
        COMMAND $<TARGET_FILE:atelier_editor_document_acceptance> --physics-test-project
        DEPENDS atelier_editor_document_acceptance USES_TERMINAL VERBATIM)
    add_custom_target(physics_examples_acceptance
        COMMAND $<TARGET_FILE:atelier_editor_document_acceptance> --physics-examples
        COMMAND $<TARGET_FILE:atelier_editor> --acceptance-physics-examples "${CMAKE_BINARY_DIR}/physics-examples-evidence"
        DEPENDS atelier_editor_document_acceptance atelier_editor USES_TERMINAL VERBATIM)
    add_custom_target(editor_sponza_acceptance
        COMMAND $<TARGET_FILE:atelier_editor_document_acceptance> --textured-obj
        COMMAND $<TARGET_FILE:atelier_editor> --acceptance-sponza "${CMAKE_BINARY_DIR}/sponza-evidence"
        DEPENDS atelier_editor_document_acceptance atelier_editor USES_TERMINAL VERBATIM)
    add_custom_target(editor_examples_acceptance
        COMMAND $<TARGET_FILE:atelier_editor_document_acceptance> --examples
        COMMAND $<TARGET_FILE:atelier_editor> --acceptance-examples "${CMAKE_BINARY_DIR}/examples-evidence"
        DEPENDS atelier_editor_document_acceptance atelier_editor USES_TERMINAL VERBATIM)
    add_custom_target(editor_dungeons_acceptance
        COMMAND $<TARGET_FILE:atelier_editor_document_acceptance> --dungeons
        COMMAND $<TARGET_FILE:atelier_editor> --acceptance-dungeons "${CMAKE_BINARY_DIR}/dungeons-evidence"
        DEPENDS atelier_editor_document_acceptance atelier_editor USES_TERMINAL VERBATIM)
    add_custom_target(editor_acceptance
        COMMAND $<TARGET_FILE:atelier_editor> --acceptance "${CMAKE_BINARY_DIR}/editor-evidence"
        DEPENDS atelier_editor USES_TERMINAL VERBATIM)
    add_custom_target(editor_ime_acceptance
        COMMAND $<TARGET_FILE:atelier_editor> --acceptance-ime "${CMAKE_BINARY_DIR}/editor-evidence"
        DEPENDS atelier_editor USES_TERMINAL VERBATIM)
    add_custom_target(editor_project_acceptance
        COMMAND $<TARGET_FILE:atelier_editor> --acceptance-project "${CMAKE_BINARY_DIR}/project-evidence"
        COMMAND $<TARGET_FILE:atelier_editor> --acceptance-source "${CMAKE_BINARY_DIR}/source-evidence"
        DEPENDS atelier_editor USES_TERMINAL VERBATIM)
    add_custom_target(editor_viewport_acceptance
        COMMAND $<TARGET_FILE:atelier_editor> --acceptance-viewport "${CMAKE_BINARY_DIR}/viewport-evidence"
        DEPENDS atelier_editor USES_TERMINAL VERBATIM)
    add_custom_target(editor_shortcuts_acceptance
        COMMAND $<TARGET_FILE:atelier_editor> --acceptance-shortcuts "${CMAKE_BINARY_DIR}/shortcut-evidence"
        DEPENDS atelier_editor USES_TERMINAL VERBATIM)
    add_custom_target(editor_edit_acceptance
        COMMAND $<TARGET_FILE:atelier_editor_document_acceptance> --viewport-edit
        COMMAND $<TARGET_FILE:atelier_editor> --acceptance-edit "${CMAKE_BINARY_DIR}/edit-mode-evidence"
        DEPENDS atelier_editor atelier_editor_document_acceptance USES_TERMINAL VERBATIM)
    add_custom_target(editor_surfaces_acceptance
        COMMAND $<TARGET_FILE:atelier_editor> --acceptance-surfaces "${CMAKE_BINARY_DIR}/surface-evidence"
        COMMAND $<TARGET_FILE:atelier_editor> --acceptance-tooltips "${CMAKE_BINARY_DIR}/tooltip-evidence"
        DEPENDS atelier_editor USES_TERMINAL VERBATIM)
    add_custom_target(editor_dialogs_acceptance
        COMMAND $<TARGET_FILE:atelier_editor> --acceptance-dialogs "${CMAKE_BINARY_DIR}/dialog-evidence"
        DEPENDS atelier_editor USES_TERMINAL VERBATIM)
    add_custom_target(editor_scene_lit_acceptance
        COMMAND $<TARGET_FILE:atelier_editor_document_acceptance> --scene-lit "${PROJECT_SOURCE_DIR}/assets/examples"
        COMMAND $<TARGET_FILE:atelier_editor> --acceptance-scene-lit "${CMAKE_BINARY_DIR}/scene-lit-editor-evidence" "${PROJECT_SOURCE_DIR}/assets/examples"
        DEPENDS atelier_editor atelier_editor_document_acceptance USES_TERMINAL VERBATIM)
endfunction()
