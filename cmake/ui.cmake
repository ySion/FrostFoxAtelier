include_guard(GLOBAL)

# The renderer consumes only this CPU protocol, never the controls/text backend.
add_library(atelier_ui_protocol INTERFACE)
target_include_directories(atelier_ui_protocol INTERFACE "${CMAKE_CURRENT_LIST_DIR}/../src")
target_compile_features(atelier_ui_protocol INTERFACE cxx_std_23)

function(atelier_add_ui)
    if(TARGET atelier_ui)
        return()
    endif()
    set(_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/..")
    configure_file("${_root}/assets/icons/LICENSE" "${CMAKE_BINARY_DIR}/LICENSE.tabler-icons.txt" COPYONLY)
    add_library(atelier_ui STATIC "${_root}/src/ui/session.cpp" "${_root}/src/ui/text.cpp" "${_root}/src/ui/icons.cpp" "${_root}/src/ui/text_edit.cpp"
        "${_root}/src/ui/style_sheet.cpp" "${_root}/src/ui/controls.cpp" "${_root}/src/ui/docking.cpp")
    target_link_libraries(atelier_ui PUBLIC atelier_ui_protocol atelier_ui_layout PRIVATE dwrite icu)
    atelier_target(atelier_ui)
    add_library(atelier_ui_language STATIC "${_root}/src/ui_language/declaration.cpp" "${_root}/src/ui_language/component.cpp"
        "${_root}/src/ui_language/live_document.cpp")
    atelier_target(atelier_ui_language)
    target_link_libraries(atelier_ui_language PUBLIC atelier_ui atelier_source)
    add_executable(atelier_ui_acceptance "${_root}/acceptance/ui/main.cpp")
    target_link_libraries(atelier_ui_acceptance PRIVATE atelier_ui)
    atelier_target(atelier_ui_acceptance)
    atelier_headless_acceptance(ui_acceptance
        COMMAND $<TARGET_FILE:atelier_ui_acceptance>
        DEPENDS atelier_ui_acceptance)
endfunction()
