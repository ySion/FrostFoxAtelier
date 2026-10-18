function(atelier_add_world)
    add_library(atelier_world STATIC
        src/material/resources.cpp
        src/world/math.cpp
        src/world/objects.cpp
        src/world/hierarchy.cpp
        src/world/lighting.cpp
        src/world/materials.cpp
        src/world/world.cpp
        src/world/execution.cpp)
    atelier_target(atelier_world)
    target_link_libraries(atelier_world PUBLIC atelier_runtime)
    target_include_directories(atelier_world PUBLIC src)
    atelier_reflect(atelier_world SOURCE src/world/objects.cpp
        HEADERS src/world/world.hpp src/material/resources.hpp)

    add_executable(atelier_world_acceptance acceptance/world/main.cpp)
    atelier_target(atelier_world_acceptance)
    target_link_libraries(atelier_world_acceptance PRIVATE atelier_world atelier_source mimalloc)
    atelier_headless_acceptance(world_acceptance
        COMMAND $<TARGET_FILE:atelier_world_acceptance>
        DEPENDS atelier_world_acceptance)
endfunction()
