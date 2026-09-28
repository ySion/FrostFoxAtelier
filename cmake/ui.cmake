include_guard(GLOBAL)

# The renderer consumes only this CPU protocol, never the controls/text backend.
add_library(atelier_ui_protocol INTERFACE)
target_include_directories(atelier_ui_protocol INTERFACE "${CMAKE_CURRENT_LIST_DIR}/../src")
target_compile_features(atelier_ui_protocol INTERFACE cxx_std_23)
