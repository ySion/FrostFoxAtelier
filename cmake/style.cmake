# Declaration style checker; built on demand, not part of `all`.
file(TO_CMAKE_PATH "$ENV{ATELIER_LLVM_ROOT}" _style_sdk)
string(REGEX REPLACE "\\..*" "" _style_llvm_major "${_pin_llvm}")
block(SCOPE_FOR VARIABLES)
    find_package(Clang REQUIRED CONFIG PATHS "${_style_sdk}/lib/cmake/clang" NO_DEFAULT_PATH)
    add_executable(atelier_style EXCLUDE_FROM_ALL tools/style/main.cpp)
    atelier_llvm_target(atelier_style)
    target_include_directories(atelier_style SYSTEM PRIVATE ${CLANG_INCLUDE_DIRS})
    target_link_libraries(atelier_style PRIVATE clangTooling)
    target_compile_definitions(atelier_style PRIVATE
        "ATELIER_STYLE_RESOURCE_DIR=\"${_style_sdk}/lib/clang/${_style_llvm_major}\"")
endblock()
