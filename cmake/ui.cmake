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
    add_library(atelier_ui STATIC "${_root}/src/ui/session.cpp" "${_root}/src/ui/text.cpp")
    target_link_libraries(atelier_ui PUBLIC atelier_ui_protocol atelier_ui_layout PRIVATE dwrite)
    atelier_target(atelier_ui)
    add_executable(atelier_ui_acceptance "${_root}/acceptance/ui/main.cpp")
    target_link_libraries(atelier_ui_acceptance PRIVATE atelier_ui)
    atelier_target(atelier_ui_acceptance)
    add_custom_target(ui_acceptance
        COMMAND $<TARGET_FILE:atelier_ui_acceptance>
        DEPENDS atelier_ui_acceptance USES_TERMINAL VERBATIM)
endfunction()
