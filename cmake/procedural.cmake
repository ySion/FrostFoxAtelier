function(atelier_add_procedural)
    add_library(atelier_procedural STATIC
        src/procedural/geometry.cpp
        src/procedural/operations.cpp
        src/procedural/instances.cpp)
    atelier_target(atelier_procedural)
    target_include_directories(atelier_procedural PUBLIC src)
    target_link_libraries(atelier_procedural PUBLIC atelier_runtime atelier_geometry)

    add_executable(atelier_procedural_acceptance acceptance/procedural/main.cpp)
    atelier_target(atelier_procedural_acceptance)
    target_link_libraries(atelier_procedural_acceptance PRIVATE atelier_procedural)
    atelier_headless_acceptance(procedural_acceptance
        COMMAND $<TARGET_FILE:atelier_procedural_acceptance>
        DEPENDS atelier_procedural_acceptance)
endfunction()
