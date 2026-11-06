# Included after the formal atelier_logging target has been defined.
if(NOT TARGET nlohmann_json::nlohmann_json)
    include("${PROJECT_SOURCE_DIR}/cmake/json.cmake")
endif()

add_executable(atelier_logging_acceptance
    "${PROJECT_SOURCE_DIR}/acceptance/logging/main.cpp"
    "${PROJECT_SOURCE_DIR}/acceptance/logging/contracts.cpp"
    "${PROJECT_SOURCE_DIR}/acceptance/logging/file_failures.cpp"
    "${PROJECT_SOURCE_DIR}/acceptance/logging/threads.cpp"
    "${PROJECT_SOURCE_DIR}/acceptance/logging/reader_cases.cpp"
    "${PROJECT_SOURCE_DIR}/acceptance/logging/dll.cpp")
atelier_target(atelier_logging_acceptance)
target_link_libraries(atelier_logging_acceptance PRIVATE atelier_logging nlohmann_json::nlohmann_json)

# This DLL deliberately does not link the logger or include native.hpp. The EXE
# supplies the resident entry point, using the same configured compiler/CRT.
add_library(atelier_logging_producer SHARED "${PROJECT_SOURCE_DIR}/acceptance/logging/producer_dll.cpp")
atelier_target(atelier_logging_producer)
target_include_directories(atelier_logging_producer PRIVATE "${PROJECT_SOURCE_DIR}/modules/core/logging/include")
target_link_libraries(atelier_logging_producer PRIVATE atelier_foundation_headers)

atelier_headless_acceptance(logging_acceptance
    COMMAND "${CMAKE_COMMAND}"
        "-DPROGRAM=$<TARGET_FILE:atelier_logging_acceptance>"
        "-DPRODUCER_DLL=$<TARGET_FILE:atelier_logging_producer>"
        "-DBUILD_DIRECTORY=${CMAKE_BINARY_DIR}"
        "-DEVIDENCE_ROOT=${CMAKE_BINARY_DIR}/logging-evidence"
        -P "${PROJECT_SOURCE_DIR}/cmake/run-logging-acceptance.cmake"
    DEPENDS atelier_logging_acceptance atelier_logging_producer)
