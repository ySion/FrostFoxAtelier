function(atelier_add_world)
    add_library(atelier_world STATIC
        src/world/math.cpp
        src/world/objects.cpp
        src/world/hierarchy.cpp
        src/world/world.cpp
        src/world/execution.cpp)
    atelier_target(atelier_world)
    target_link_libraries(atelier_world PUBLIC atelier_runtime)
    target_include_directories(atelier_world PUBLIC src)
    atelier_reflect(atelier_world SOURCE src/world/objects.cpp HEADERS src/world/world.hpp)

    # Meshes, lights, the Environment and material resources over the World core.
    add_library(atelier_presentation STATIC
        src/material/resources.cpp
        src/presentation/objects.cpp
        src/presentation/lighting.cpp
        src/presentation/materials.cpp
        src/presentation/scene.cpp)
    atelier_target(atelier_presentation)
    target_link_libraries(atelier_presentation PUBLIC atelier_world atelier_geometry)
    atelier_reflect(atelier_presentation SOURCE src/presentation/objects.cpp
        HEADERS src/presentation/presentation.hpp src/material/resources.hpp)

    add_executable(atelier_world_acceptance acceptance/world/main.cpp)
    atelier_target(atelier_world_acceptance)
    target_link_libraries(atelier_world_acceptance PRIVATE atelier_presentation atelier_source mimalloc)
    atelier_headless_acceptance(world_acceptance
        COMMAND $<TARGET_FILE:atelier_world_acceptance>
        DEPENDS atelier_world_acceptance)
endfunction()
