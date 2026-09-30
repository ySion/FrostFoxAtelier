add_executable(atelier_foundation_acceptance acceptance/foundation/main.cpp)
atelier_target(atelier_foundation_acceptance)
target_link_libraries(atelier_foundation_acceptance PRIVATE atelier_runtime)
target_include_directories(atelier_foundation_acceptance PRIVATE "${PROJECT_SOURCE_DIR}/acceptance")
atelier_reflect(atelier_foundation_acceptance
    SOURCE acceptance/foundation/main.cpp BASE_DIR acceptance
    HEADERS acceptance/foundation/fixture.hpp)
add_custom_target(foundation_acceptance
    COMMAND $<TARGET_FILE:atelier_foundation_acceptance>
    DEPENDS atelier_foundation_acceptance USES_TERMINAL VERBATIM)

# Explicit performance sampling; the ordinary acceptance target remains focused
# on the shared value/lifetime consumer rather than timing-dependent thresholds.
add_executable(atelier_string_measure EXCLUDE_FROM_ALL acceptance/foundation/string_measure.cpp)
atelier_target(atelier_string_measure)
target_link_libraries(atelier_string_measure PRIVATE atelier_foundation mimalloc)

add_executable(atelier_array_measure EXCLUDE_FROM_ALL acceptance/foundation/array_measure.cpp)
atelier_target(atelier_array_measure)
target_link_libraries(atelier_array_measure PRIVATE atelier_foundation mimalloc)
