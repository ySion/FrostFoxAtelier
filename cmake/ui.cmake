include_guard(GLOBAL)

# The renderer consumes only this CPU protocol, never the controls/text backend.
add_library(atelier_ui_protocol INTERFACE)
target_include_directories(atelier_ui_protocol INTERFACE "${PROJECT_SOURCE_DIR}/modules/gui/ui_protocol/include")
target_compile_features(atelier_ui_protocol INTERFACE cxx_std_23)

function(atelier_add_ui)
    if(TARGET atelier_ui)
        return()
    endif()
    set(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..")
    configure_file("${_root}/assets/icons/LICENSE" "${CMAKE_BINARY_DIR}/LICENSE.tabler-icons.txt" COPYONLY)
    add_library(atelier_ui STATIC "${_root}/modules/gui/ui/src/session.cpp" "${_root}/modules/gui/ui/src/text.cpp" "${_root}/modules/gui/ui/src/icons.cpp" "${_root}/modules/gui/ui/src/text_edit.cpp"
        "${_root}/modules/gui/ui/src/style_sheet.cpp" "${_root}/modules/gui/ui/src/controls.cpp" "${_root}/modules/gui/ui/src/docking.cpp")
    target_include_directories(atelier_ui PUBLIC "${PROJECT_SOURCE_DIR}/modules/gui/ui/include"
        PRIVATE "${PROJECT_SOURCE_DIR}/modules/gui/ui/internal")
    target_link_libraries(atelier_ui PUBLIC atelier_ui_protocol atelier_ui_layout atelier_foundation_headers PRIVATE atelier_platform dwrite icu)
    atelier_target(atelier_ui)
    add_library(atelier_ui_language STATIC "${_root}/modules/gui/ui_language/src/declaration.cpp" "${_root}/modules/gui/ui_language/src/component.cpp"
        "${_root}/modules/gui/ui_language/src/live_document.cpp")
    atelier_target(atelier_ui_language)
    target_include_directories(atelier_ui_language PUBLIC "${PROJECT_SOURCE_DIR}/modules/gui/ui_language/include")
    target_link_libraries(atelier_ui_language PUBLIC atelier_ui atelier_source PRIVATE atelier_platform)
    add_executable(atelier_ui_acceptance "${_root}/acceptance/ui/main.cpp")
    target_include_directories(atelier_ui_acceptance PRIVATE "${PROJECT_SOURCE_DIR}/modules/gui/ui/internal")
    target_link_libraries(atelier_ui_acceptance PRIVATE atelier_ui_language)
    atelier_target(atelier_ui_acceptance)
    atelier_headless_acceptance(ui_acceptance
        COMMAND $<TARGET_FILE:atelier_ui_acceptance>
        DEPENDS atelier_ui_acceptance)
endfunction()
