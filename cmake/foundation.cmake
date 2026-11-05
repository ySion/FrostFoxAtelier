add_executable(atelier_foundation_acceptance acceptance/foundation/main.cpp acceptance/foundation/associative.cpp
    acceptance/foundation/lifecycle.cpp acceptance/foundation/frame_timing.cpp acceptance/foundation/tasks.cpp)
atelier_target(atelier_foundation_acceptance)
target_link_libraries(atelier_foundation_acceptance PRIVATE atelier_runtime)
target_include_directories(atelier_foundation_acceptance PRIVATE "${PROJECT_SOURCE_DIR}/acceptance")
atelier_reflect(atelier_foundation_acceptance
    SOURCE acceptance/foundation/main.cpp BASE_DIR acceptance
    HEADERS acceptance/foundation/fixture.hpp)

# Strict buffer-lifetime checks share the foundation acceptance entry point.
add_executable(atelier_string_lifetime EXCLUDE_FROM_ALL acceptance/foundation/string_lifetime.cpp)
atelier_target(atelier_string_lifetime)
target_link_libraries(atelier_string_lifetime PRIVATE atelier_foundation mimalloc)

add_executable(atelier_array_lifetime EXCLUDE_FROM_ALL acceptance/foundation/array_lifetime.cpp)
atelier_target(atelier_array_lifetime)
target_link_libraries(atelier_array_lifetime PRIVATE atelier_foundation mimalloc)

atelier_headless_acceptance(foundation_acceptance
    COMMAND $<TARGET_FILE:atelier_foundation_acceptance>
    COMMAND $<TARGET_FILE:atelier_array_lifetime>
    COMMAND $<TARGET_FILE:atelier_string_lifetime>
    DEPENDS atelier_foundation_acceptance atelier_array_lifetime atelier_string_lifetime)
