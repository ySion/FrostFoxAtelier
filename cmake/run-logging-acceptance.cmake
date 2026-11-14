cmake_minimum_required(VERSION 4.2)

string(TIMESTAMP stamp "%Y%m%d-%H%M%S" UTC)
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef nonce)
set(evidence "${EVIDENCE_ROOT}/${stamp}-${nonce}")
file(MAKE_DIRECTORY "${evidence}")
message(STATUS "Logging acceptance evidence: ${evidence}")

# The logger permits only one successful Run per process.
foreach(name roundtrip file-failures)
    execute_process(COMMAND "${PROGRAM}" "${name}" "${evidence}/${name}"
        WORKING_DIRECTORY "${BUILD_DIRECTORY}"
        TIMEOUT 60 RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    file(WRITE "${evidence}/${name}.log" "${out}\n${err}\nexit=${result}\n")
    if(NOT "${result}" STREQUAL "0" OR NOT out MATCHES "Logging acceptance: PASS case=${name}")
        message(FATAL_ERROR "Logging case ${name} failed (${result}):\n${out}\n${err}")
    endif()
    message(STATUS "${name}: PASS (exit=${result})")
endforeach()
message(STATUS "Logging acceptance: PASS; evidence=${evidence}")
