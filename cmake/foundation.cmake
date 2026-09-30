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
