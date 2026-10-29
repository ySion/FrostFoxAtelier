include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/json.cmake)

add_library(atelier_assets STATIC
    "${PROJECT_SOURCE_DIR}/src/assets/builtin.cpp"
    "${PROJECT_SOURCE_DIR}/src/assets/builtin_materials.cpp"
    "${PROJECT_SOURCE_DIR}/src/assets/content_digest.cpp"
    "${PROJECT_SOURCE_DIR}/src/assets/reader.cpp"
    "${PROJECT_SOURCE_DIR}/src/assets/file.cpp"
    "${PROJECT_SOURCE_DIR}/src/assets/image_decode.cpp"
    "${PROJECT_SOURCE_DIR}/src/assets/obj_import.cpp"
    "${PROJECT_SOURCE_DIR}/src/assets/gltf_import.cpp")
atelier_target(atelier_assets)
target_include_directories(atelier_assets PUBLIC "${PROJECT_SOURCE_DIR}/src")
# No editor, presentation, physics, Renderer or device dependency belongs here.
target_link_libraries(atelier_assets PUBLIC atelier_runtime atelier_geometry atelier_rendering atelier_source atelier_material
    PRIVATE atelier_foundation windowscodecs ole32 nlohmann_json::nlohmann_json)

add_executable(atelier_mesh_import_acceptance "${PROJECT_SOURCE_DIR}/acceptance/assets/mesh_import.cpp")
atelier_target(atelier_mesh_import_acceptance)
target_link_libraries(atelier_mesh_import_acceptance PRIVATE atelier_assets nlohmann_json::nlohmann_json)
atelier_headless_acceptance(mesh_import_acceptance
    COMMAND $<TARGET_FILE:atelier_mesh_import_acceptance>
    DEPENDS atelier_mesh_import_acceptance)
