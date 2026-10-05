include(${CMAKE_CURRENT_LIST_DIR}/json.cmake)

function(atelier_add_editor)
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
        "${PROJECT_SOURCE_DIR}/src/editor/assets.cpp"
        "${PROJECT_SOURCE_DIR}/src/editor/project.cpp"
        "${PROJECT_SOURCE_DIR}/src/editor/scene.cpp"
        "${PROJECT_SOURCE_DIR}/src/editor/session.cpp"
        "${PROJECT_SOURCE_DIR}/src/editor/camera.cpp")
    atelier_target(atelier_editor_core)
    target_include_directories(atelier_editor_core PUBLIC "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(atelier_editor_core PUBLIC atelier_world atelier_ui_language PRIVATE ole32 nlohmann_json::nlohmann_json)
    add_library(atelier_editor_application STATIC
        "${PROJECT_SOURCE_DIR}/src/application/window.cpp"
        "${PROJECT_SOURCE_DIR}/src/application/popup.cpp"
        "${PROJECT_SOURCE_DIR}/src/application/application.cpp"
        "${PROJECT_SOURCE_DIR}/src/editor/workspace.cpp"
        "${PROJECT_SOURCE_DIR}/src/editor/asset_browser.cpp"
        "${PROJECT_SOURCE_DIR}/src/application/project.cpp"
        "${PROJECT_SOURCE_DIR}/src/editor/control_workbench.cpp")
    atelier_target(atelier_editor_application)
    add_dependencies(atelier_editor_application atelier_editor_build_info)
    target_include_directories(atelier_editor_application PUBLIC "${CMAKE_BINARY_DIR}/generated")
    target_compile_definitions(atelier_editor_application PUBLIC UNICODE _UNICODE)
    target_compile_definitions(atelier_editor_application PRIVATE "ATELIER_UI_AUTHORING_PATH=L\"${PROJECT_SOURCE_DIR}/assets/editor.ui.source\"")
    target_link_libraries(atelier_editor_application PUBLIC atelier_editor_core atelier_ui atelier_rhi)
    add_executable(atelier_editor
        "${PROJECT_SOURCE_DIR}/src/application/main.cpp"
        "${PROJECT_SOURCE_DIR}/acceptance/editor/window.cpp")
    atelier_target(atelier_editor)
    target_link_libraries(atelier_editor PRIVATE atelier_editor_application atelier_ui atelier_renderer user32 ole32 shell32 windowscodecs imm32 uuid gdi32)
    atelier_deploy_rhi_runtime(atelier_editor)
    add_executable(atelier_editor_document_acceptance "${PROJECT_SOURCE_DIR}/acceptance/editor/document.cpp")
    atelier_target(atelier_editor_document_acceptance)
    target_link_libraries(atelier_editor_document_acceptance PRIVATE atelier_editor_application nlohmann_json::nlohmann_json)
    add_custom_target(editor_document_acceptance
        COMMAND $<TARGET_FILE:atelier_editor_document_acceptance>
        DEPENDS atelier_editor_document_acceptance USES_TERMINAL VERBATIM)
    add_custom_target(editor_acceptance
        COMMAND $<TARGET_FILE:atelier_editor_document_acceptance>
        COMMAND $<TARGET_FILE:atelier_editor> --acceptance "${CMAKE_BINARY_DIR}/editor-evidence"
        DEPENDS atelier_editor atelier_editor_document_acceptance USES_TERMINAL VERBATIM)
    add_custom_target(editor_ime_acceptance
        COMMAND $<TARGET_FILE:atelier_editor> --acceptance-ime "${CMAKE_BINARY_DIR}/editor-evidence"
        DEPENDS atelier_editor USES_TERMINAL VERBATIM)
    add_custom_target(editor_project_acceptance
        COMMAND $<TARGET_FILE:atelier_editor> --acceptance-project "${CMAKE_BINARY_DIR}/project-evidence"
        DEPENDS atelier_editor USES_TERMINAL VERBATIM)
    add_custom_target(editor_viewport_acceptance
        COMMAND $<TARGET_FILE:atelier_editor> --acceptance-viewport "${CMAKE_BINARY_DIR}/viewport-evidence"
        DEPENDS atelier_editor USES_TERMINAL VERBATIM)
endfunction()
