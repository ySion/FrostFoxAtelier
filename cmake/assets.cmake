include_guard(GLOBAL)

add_library(atelier_assets STATIC
    "${PROJECT_SOURCE_DIR}/modules/engine/assets/src/builtin.cpp"
    "${PROJECT_SOURCE_DIR}/modules/engine/assets/src/builtin_materials.cpp"
    "${PROJECT_SOURCE_DIR}/modules/engine/assets/src/content_digest.cpp"
    "${PROJECT_SOURCE_DIR}/modules/engine/assets/src/reader.cpp"
    "${PROJECT_SOURCE_DIR}/modules/engine/assets/src/file.cpp"
    "${PROJECT_SOURCE_DIR}/modules/engine/assets/src/image_decode.cpp"
    "${PROJECT_SOURCE_DIR}/modules/engine/assets/src/png.cpp"
    "${PROJECT_SOURCE_DIR}/modules/engine/assets/src/obj_import.cpp"
    "${PROJECT_SOURCE_DIR}/modules/engine/assets/src/gltf_import.cpp")
atelier_target(atelier_assets)
target_include_directories(atelier_assets PUBLIC "${PROJECT_SOURCE_DIR}/modules/engine/assets/include")
# No editor, presentation, physics, Renderer or device dependency belongs here.
target_link_libraries(atelier_assets PUBLIC atelier_runtime atelier_geometry atelier_rendering atelier_source atelier_material
    atelier_platform atelier_core_json PRIVATE atelier_foundation windowscodecs ole32)

target_sources(atelier_world_acceptance PRIVATE "${PROJECT_SOURCE_DIR}/acceptance/assets/mesh_import.cpp")
target_link_libraries(atelier_world_acceptance PRIVATE atelier_assets)
