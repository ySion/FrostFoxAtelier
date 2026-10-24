# Headless commands keep their existing binary-directory cwd (fonts and fixtures),
# but get private TEMP/TMP and logs. No console pool: Ninja may run them in parallel.
function(atelier_headless_acceptance target)
    cmake_parse_arguments(PARSE_ARGV 1 A "" "" "DEPENDS")
    set(_directory "${CMAKE_BINARY_DIR}/acceptance-headless/${target}")
    set(_script "set(_directory [==[${_directory}]==])\n")
    string(APPEND _script "include([==[${PROJECT_SOURCE_DIR}/cmake/run-headless-acceptance.cmake]==])\n")
    set(_command)
    foreach(_argument IN LISTS A_UNPARSED_ARGUMENTS)
        if(_argument STREQUAL "COMMAND")
            if(_command)
                string(APPEND _script "atelier_run(${_command})\n")
            endif()
            set(_command)
        else()
            string(APPEND _command " [==[${_argument}]==]")
        endif()
    endforeach()
    string(APPEND _script "atelier_run(${_command})\natelier_finish(PASS)\n")
    file(GENERATE OUTPUT "${_directory}/run.cmake" CONTENT "${_script}")
    add_custom_target(${target}
        COMMAND "${CMAKE_COMMAND}" -P "${_directory}/run.cmake"
        DEPENDS ${A_DEPENDS}
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}" VERBATIM)
    set_property(GLOBAL APPEND PROPERTY ATELIER_HEADLESS_ACCEPTANCE "${target}")
endfunction()
