include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/json.cmake)

add_library(atelier_assets STATIC
    "${PROJECT_SOURCE_DIR}/modules/engine/assets/src/builtin.cpp"
    "${PROJECT_SOURCE_DIR}/modules/engine/assets/src/builtin_materials.cpp"
    "${PROJECT_SOURCE_DIR}/modules/engine/assets/src/content_digest.cpp"
    "${PROJECT_SOURCE_DIR}/modules/engine/assets/src/reader.cpp"
    "${PROJECT_SOURCE_DIR}/modules/engine/assets/src/file.cpp"
    "${PROJECT_SOURCE_DIR}/modules/engine/assets/src/image_decode.cpp"
    "${PROJECT_SOURCE_DIR}/modules/engine/assets/src/obj_import.cpp"
    "${PROJECT_SOURCE_DIR}/modules/engine/assets/src/gltf_import.cpp")
atelier_target(atelier_assets)
target_include_directories(atelier_assets PUBLIC "${PROJECT_SOURCE_DIR}/modules/engine/assets/include")
# No editor, presentation, physics, Renderer or device dependency belongs here.
target_link_libraries(atelier_assets PUBLIC atelier_runtime atelier_geometry atelier_rendering atelier_source atelier_material
    PRIVATE atelier_foundation atelier_platform windowscodecs ole32 nlohmann_json::nlohmann_json)

add_executable(atelier_mesh_import_acceptance "${PROJECT_SOURCE_DIR}/acceptance/assets/mesh_import.cpp")
atelier_target(atelier_mesh_import_acceptance)
target_link_libraries(atelier_mesh_import_acceptance PRIVATE atelier_assets nlohmann_json::nlohmann_json)
atelier_headless_acceptance(mesh_import_acceptance
    COMMAND $<TARGET_FILE:atelier_mesh_import_acceptance>
    DEPENDS atelier_mesh_import_acceptance)
