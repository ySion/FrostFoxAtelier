include_guard(GLOBAL)
add_library(atelier_mikktspace STATIC "${PROJECT_SOURCE_DIR}/third_party/mikktspace/mikktspace.c")
set_target_properties(atelier_mikktspace PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
target_include_directories(atelier_mikktspace PUBLIC "${PROJECT_SOURCE_DIR}/third_party/mikktspace")

add_library(atelier_geometry STATIC "${PROJECT_SOURCE_DIR}/modules/engine/geometry/src/tangents.cpp")
atelier_target(atelier_geometry)
target_include_directories(atelier_geometry PUBLIC "${PROJECT_SOURCE_DIR}/modules/engine/geometry/include")
target_link_libraries(atelier_geometry PUBLIC atelier_math atelier_foundation_headers PRIVATE atelier_mikktspace)
