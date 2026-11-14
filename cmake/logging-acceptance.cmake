# Included after the formal atelier_logging target has been defined.
add_executable(atelier_logging_acceptance
    "${PROJECT_SOURCE_DIR}/acceptance/logging/main.cpp"
    "${PROJECT_SOURCE_DIR}/acceptance/logging/contracts.cpp"
    "${PROJECT_SOURCE_DIR}/acceptance/logging/file_failures.cpp")
atelier_target(atelier_logging_acceptance)
target_link_libraries(atelier_logging_acceptance PRIVATE atelier_logging atelier_core_json)

atelier_headless_acceptance(logging_acceptance
    COMMAND "${CMAKE_COMMAND}"
        "-DPROGRAM=$<TARGET_FILE:atelier_logging_acceptance>"
        "-DBUILD_DIRECTORY=${CMAKE_BINARY_DIR}"
        "-DEVIDENCE_ROOT=${CMAKE_BINARY_DIR}/logging-evidence"
        -P "${PROJECT_SOURCE_DIR}/cmake/run-logging-acceptance.cmake"
    DEPENDS atelier_logging_acceptance)
