function(atelier_add_procedural)
    add_library(atelier_procedural STATIC
        src/procedural/geometry.cpp
        src/procedural/operations.cpp
        src/procedural/triangles.cpp
        src/procedural/instances.cpp
        src/procedural/bvh.cpp
        src/procedural/graph.cpp
        src/procedural/nodes.cpp
        src/procedural/evaluator.cpp)
    atelier_target(atelier_procedural)
    target_include_directories(atelier_procedural PUBLIC src)
    target_link_libraries(atelier_procedural PUBLIC atelier_runtime atelier_geometry atelier_source)
    atelier_reflect(atelier_procedural SOURCE src/procedural/graph.cpp
        HEADERS src/procedural/geometry.hpp src/procedural/graph.hpp src/procedural/nodes.hpp)

    add_executable(atelier_procedural_acceptance acceptance/procedural/main.cpp)
    atelier_target(atelier_procedural_acceptance)
    target_link_libraries(atelier_procedural_acceptance PRIVATE atelier_procedural)
    atelier_headless_acceptance(procedural_acceptance
        COMMAND $<TARGET_FILE:atelier_procedural_acceptance>
        DEPENDS atelier_procedural_acceptance)
endfunction()
