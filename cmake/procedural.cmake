function(atelier_add_procedural)
    add_library(atelier_procedural STATIC
        modules/engine/procedural/src/geometry.cpp
        modules/engine/procedural/src/operations.cpp
        modules/engine/procedural/src/topology.cpp
        modules/engine/procedural/src/boolean.cpp
        modules/engine/procedural/src/constructors.cpp
        modules/engine/procedural/src/triangles.cpp
        modules/engine/procedural/src/volume.cpp
        modules/engine/procedural/src/instances.cpp
        modules/engine/procedural/src/bvh.cpp
        modules/engine/procedural/src/modeling.cpp
        modules/engine/procedural/src/graph.cpp
        modules/engine/procedural/src/nodes.cpp
        modules/engine/procedural/src/evaluator.cpp)
    atelier_target(atelier_procedural)
    # Volume/BVH error-bound proofs rely on separate multiply/add rounding.
    set_source_files_properties(
        modules/engine/procedural/src/triangles.cpp
        modules/engine/procedural/src/bvh.cpp
        modules/engine/procedural/src/volume.cpp
        PROPERTIES COMPILE_OPTIONS /clang:-ffp-contract=off)
    target_include_directories(atelier_procedural PUBLIC modules/engine/procedural/include)
    target_link_libraries(atelier_procedural PUBLIC atelier_runtime atelier_geometry atelier_source)
    atelier_reflect(atelier_procedural SOURCE modules/engine/procedural/src/graph.cpp
        BASE_DIR modules/engine/procedural/include
        HEADERS modules/engine/procedural/include/procedural/geometry.hpp modules/engine/procedural/include/procedural/bvh.hpp modules/engine/procedural/include/procedural/graph.hpp modules/engine/procedural/include/procedural/nodes.hpp)

    add_executable(atelier_procedural_acceptance
        acceptance/procedural/main.cpp
        acceptance/procedural/boolean_profile.cpp)
    atelier_target(atelier_procedural_acceptance)
    target_compile_options(atelier_procedural_acceptance PRIVATE /clang:-ffp-contract=off)
    target_link_libraries(atelier_procedural_acceptance PRIVATE atelier_procedural)
    atelier_headless_acceptance(procedural_acceptance
        COMMAND $<TARGET_FILE:atelier_procedural_acceptance>
        DEPENDS atelier_procedural_acceptance)
endfunction()
