# Build-time only. Reuse the pinned SDK and its Release/static CRT ABI.
file(TO_CMAKE_PATH "$ENV{ATELIER_LLVM_ROOT}" _reflection_sdk)
# LLVMConfig sets CMAKE_MSVC_RUNTIME_LIBRARY; keep that SDK default out of
# later engine and third-party targets while preserving the tool's explicit ABI.
block(SCOPE_FOR VARIABLES)
    find_package(Clang REQUIRED CONFIG PATHS "${_reflection_sdk}/lib/cmake/clang" NO_DEFAULT_PATH)
    add_executable(atelier_reflect tools/reflection/main.cpp)
    atelier_llvm_target(atelier_reflect)
    target_include_directories(atelier_reflect SYSTEM PRIVATE ${CLANG_INCLUDE_DIRS})
    target_link_libraries(atelier_reflect PRIVATE clangTooling)
endblock()

function(_atelier_reflection_json out value)
    string(REPLACE "\\" "\\\\" value "${value}")
    string(REPLACE "\"" "\\\"" value "${value}")
    set(${out} "\"${value}\"" PARENT_SCOPE)
endfunction()

# SOURCE selects a real target compile command; headers use those exact settings.
# Generated headers retain their path relative to the explicit public include root.
function(atelier_reflect target)
    cmake_parse_arguments(PARSE_ARGV 1 AR "" "SOURCE;BASE_DIR" "HEADERS")
    if(AR_UNPARSED_ARGUMENTS OR NOT AR_SOURCE OR NOT AR_HEADERS OR NOT AR_BASE_DIR)
        message(FATAL_ERROR "atelier_reflect requires SOURCE, BASE_DIR and HEADERS")
    endif()
    cmake_path(ABSOLUTE_PATH AR_BASE_DIR BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE)
    cmake_path(ABSOLUTE_PATH AR_SOURCE BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE)
    set(_output "${CMAKE_CURRENT_BINARY_DIR}/generated/${target}")
    set(_cpp "${_output}/reflection.gen.cpp")
    set(_depfile "${_output}/reflection.d")
    set(_config "${_output}/reflection.json")
    set(_headers_json)
    set(_outputs "${_cpp}")
    set(_inputs)
    foreach(_header IN LISTS AR_HEADERS)
        cmake_path(ABSOLUTE_PATH _header BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE)
        cmake_path(RELATIVE_PATH _header BASE_DIRECTORY "${AR_BASE_DIR}" OUTPUT_VARIABLE _include)
        if(_include MATCHES "^\\.\\." OR NOT _include MATCHES "\\.hpp$")
            message(FATAL_ERROR "Reflection headers must be .hpp files inside BASE_DIR: ${_header}")
        endif()
        string(REGEX REPLACE "\\.hpp$" ".gen.hpp" _generated "${_include}")
        set(_generated "${_output}/${_generated}")
        _atelier_reflection_json(_path_json "${_header}")
        _atelier_reflection_json(_include_json "${_include}")
        _atelier_reflection_json(_generated_json "${_generated}")
        list(APPEND _headers_json "{\"path\":${_path_json},\"include\":${_include_json},\"output\":${_generated_json}}")
        list(APPEND _outputs "${_generated}")
        list(APPEND _inputs "${_header}")
    endforeach()
    # Retire moved/removed outputs so this target's stale headers cannot shadow
    # the current producer through its public generated include directory.
    file(GLOB_RECURSE _previous_headers "${_output}/*.gen.hpp")
    list(REMOVE_ITEM _previous_headers ${_outputs})
    if(_previous_headers)
        file(REMOVE ${_previous_headers})
    endif()
    list(JOIN _headers_json ",\n" _headers_json)
    _atelier_reflection_json(_source_json "${AR_SOURCE}")
    _atelier_reflection_json(_database_json "${CMAKE_BINARY_DIR}")
    _atelier_reflection_json(_cpp_json "${_cpp}")
    _atelier_reflection_json(_depfile_json "${_depfile}")
    # -resource-dir points at the directory above include/.
    get_filename_component(_resource_dir "${_reflection_sdk}/lib/clang/${_pin_llvm}/include" DIRECTORY)
    if(NOT IS_DIRECTORY "${_resource_dir}")
        string(REGEX REPLACE "\\..*" "" _major "${_pin_llvm}")
        set(_resource_dir "${_reflection_sdk}/lib/clang/${_major}")
    endif()
    _atelier_reflection_json(_resource_json "${_resource_dir}")
    file(GENERATE OUTPUT "${_config}" CONTENT
        "{\"source\":${_source_json},\"database\":${_database_json},\"cpp\":${_cpp_json},\"depfile\":${_depfile_json},\"resource_dir\":${_resource_json},\"headers\":[${_headers_json}]}\n")
    add_custom_command(OUTPUT ${_outputs}
        COMMAND $<TARGET_FILE:atelier_reflect> "${_config}"
        DEPENDS atelier_reflect "${_config}" "${CMAKE_BINARY_DIR}/compile_commands.json" ${_inputs}
        DEPFILE "${_depfile}"
        COMMENT "Generating ${target} reflection" VERBATIM)
    target_sources(${target} PRIVATE "${_cpp}")
    target_include_directories(${target} PUBLIC "$<BUILD_INTERFACE:${_output}>")
endfunction()

atelier_reflect(atelier_runtime_acceptance SOURCE acceptance/reflection/main.cpp
    BASE_DIR "${PROJECT_SOURCE_DIR}/acceptance" HEADERS acceptance/reflection/fixture.hpp)
