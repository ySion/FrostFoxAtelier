function(atelier_add_world)
    add_library(atelier_world STATIC
        modules/engine/world/src/math.cpp
        modules/engine/world/src/objects.cpp
        modules/engine/world/src/hierarchy.cpp
        modules/engine/world/src/world.cpp
        modules/engine/world/src/execution.cpp)
    atelier_target(atelier_world)
    target_link_libraries(atelier_world PUBLIC atelier_runtime)
    target_include_directories(atelier_world PUBLIC modules/engine/world/include
        PRIVATE modules/engine/world/internal)
    atelier_reflect(atelier_world SOURCE modules/engine/world/src/objects.cpp
        BASE_DIR modules/engine/world/include HEADERS modules/engine/world/include/world/world.hpp)

    # Meshes, lights and the Environment consume the World core and material resources.
    add_library(atelier_presentation STATIC
        modules/engine/presentation/src/objects.cpp
        modules/engine/presentation/src/camera.cpp
        modules/engine/presentation/src/lighting.cpp
        modules/engine/presentation/src/materials.cpp
        modules/engine/presentation/src/scene.cpp)
    atelier_target(atelier_presentation)
    target_include_directories(atelier_presentation PUBLIC modules/engine/presentation/include)
    target_link_libraries(atelier_presentation PUBLIC atelier_world atelier_geometry atelier_rendering atelier_material atelier_procedural)
    atelier_reflect(atelier_presentation SOURCE modules/engine/presentation/src/objects.cpp
        BASE_DIR modules/engine/presentation/include HEADERS modules/engine/presentation/include/presentation/presentation.hpp)

    add_executable(atelier_world_acceptance acceptance/world/main.cpp)
    atelier_target(atelier_world_acceptance)
    target_include_directories(atelier_world_acceptance PRIVATE modules/engine/world/internal)
    target_link_libraries(atelier_world_acceptance PRIVATE atelier_presentation atelier_source mimalloc)
    atelier_headless_acceptance(world_acceptance
        COMMAND $<TARGET_FILE:atelier_world_acceptance>
        DEPENDS atelier_world_acceptance)
endfunction()
