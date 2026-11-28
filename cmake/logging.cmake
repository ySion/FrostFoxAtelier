include("${CMAKE_CURRENT_LIST_DIR}/quill.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/core-json.cmake")
add_library(atelier_logging STATIC
    "${PROJECT_SOURCE_DIR}/modules/core/logging/src/logging.cpp"
    "${PROJECT_SOURCE_DIR}/modules/core/logging/src/format.cpp"
    "${PROJECT_SOURCE_DIR}/modules/core/logging/src/reader.cpp")
atelier_target(atelier_logging)
target_compile_features(atelier_logging PUBLIC cxx_std_23)
target_include_directories(atelier_logging PUBLIC "${PROJECT_SOURCE_DIR}/modules/core/logging/include")
# Only native.hpp exposes Quill headers. The owning/domain API is standard-only.
target_link_libraries(atelier_logging PUBLIC quill::quill atelier_foundation PRIVATE atelier_core_json bcrypt)
# CMake normally passes absolute translation-unit paths. Preserve the original
# compilation working directory for a compiler-provided relative file spelling.
target_compile_definitions(atelier_logging PRIVATE ATELIER_LOGGING_SOURCE_ROOT="${CMAKE_BINARY_DIR}")

include("${CMAKE_CURRENT_LIST_DIR}/logging-acceptance.cmake")
