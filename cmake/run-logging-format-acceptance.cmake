# Included only by the dedicated logging runner after the linked public consumer
# has passed. Compile-only probes use CMake's actual generated compiler commands,
# including transitive headers, CRT, warning policy and Debug/Release definitions.
file(READ "${BUILD_DIRECTORY}/compile_commands.json" database)
string(JSON command_count LENGTH "${database}")
math(EXPR last_command "${command_count} - 1")

function(check_logging_format probe expected_reason)
    set(source "${SOURCE_DIRECTORY}/acceptance/logging/format_${probe}.cpp")
    cmake_path(NORMAL_PATH source)
    set(found FALSE)
    foreach(index RANGE 0 ${last_command})
        string(JSON candidate GET "${database}" ${index} file)
        cmake_path(NORMAL_PATH candidate)
        if(NOT candidate STREQUAL source)
            continue()
        endif()
        if(found)
            message(FATAL_ERROR "Ambiguous compile commands for ${source}.")
        endif()
        set(found TRUE)
        string(JSON command GET "${database}" ${index} command)
        string(JSON directory GET "${database}" ${index} directory)
        string(JSON object GET "${database}" ${index} output)
    endforeach()
    if(NOT found)
        message(FATAL_ERROR "Missing registered public format probe: ${source}")
    endif()
    get_filename_component(object_directory "${object}" DIRECTORY)
    file(MAKE_DIRECTORY "${object_directory}")
    # Keep CMake list escaping out of compiler arguments. In particular an /Fd
    # directory ending in a backslash must not absorb the following -c switch.
    # clang-cl reads the original generated argument string from a response file.
    if(NOT command MATCHES "^(\"[^\"]+\"|[^ ]+) (.*)$")
        message(FATAL_ERROR "Cannot identify the compiler in ${command}")
    endif()
    set(compiler "${CMAKE_MATCH_1}")
    set(arguments "${CMAKE_MATCH_2}")
    string(REGEX REPLACE "^\"|\"$" "" compiler "${compiler}")
    set(response "${evidence}/format-${probe}.rsp")
    file(WRITE "${response}" "${arguments}\n")
    file(WRITE "${evidence}/format-${probe}.command.txt" "cwd=${directory}\n${command}\n")
    execute_process(COMMAND "${compiler}" "@${response}" WORKING_DIRECTORY "${directory}" TIMEOUT 60
        RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    file(WRITE "${evidence}/format-${probe}.log" "${out}${err}\nexit=${result}\n")
    if(probe STREQUAL "good")
        if(NOT "${result}" STREQUAL "0")
            message(FATAL_ERROR "Public positive format probe failed, not a valid negative control: ${evidence}/format-${probe}.log")
        endif()
    else()
        # clang-cl's ordinary compilation diagnostic exit is 1. Timeouts,
        # missing tools and crashes are not acceptable compile-time rejections.
        if(NOT "${result}" STREQUAL "1" OR NOT "${out}${err}" MATCHES "format_${probe}\\.cpp" OR
            NOT "${out}${err}" MATCHES "${expected_reason}")
            message(FATAL_ERROR "Format ${probe} did not fail for its declared constraint: ${evidence}/format-${probe}.log")
        endif()
    endif()
    file(APPEND "${evidence}/results.csv" "format-${probe},compile-contract,${result},PASS\n")
    message(STATUS "Public native format ${probe}: expected compilation result verified")
endfunction()

check_logging_format(good "")
check_logging_format(bad_type "invalid format specifier|invalid_type_specifier|invalid type specifier")
check_logging_format(bad_arity "argument not found|argument_not_found")
