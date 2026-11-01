cmake_minimum_required(VERSION 4.2)

foreach(required PROGRAM PRODUCER_DLL BUILD_DIRECTORY EVIDENCE_ROOT)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "Logging acceptance requires ${required}.")
    endif()
endforeach()
if(NOT EXISTS "${PROGRAM}" OR NOT EXISTS "${PRODUCER_DLL}")
    message(FATAL_ERROR "Build the formal logging consumer and producer DLL first.")
endif()

string(TIMESTAMP stamp "%Y%m%d-%H%M%S" UTC)
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef nonce)
set(evidence "${EVIDENCE_ROOT}/${stamp}-${nonce}")
file(MAKE_DIRECTORY "${evidence}")
file(WRITE "${evidence}/results.csv" "Case,ExpectedExit,ActualExit,Result\n")
message(STATUS "Logging acceptance evidence: ${evidence}")

function(run_logging_case name timeout)
    # The logger allows only one successful Run per process.
    execute_process(COMMAND "${PROGRAM}" "${name}" "${evidence}/${name}" "${PRODUCER_DLL}"
        WORKING_DIRECTORY "${BUILD_DIRECTORY}"
        TIMEOUT ${timeout} RESULT_VARIABLE result
        OUTPUT_FILE "${evidence}/${name}.stdout.log" ERROR_FILE "${evidence}/${name}.stderr.log")
    file(READ "${evidence}/${name}.stdout.log" out)
    file(READ "${evidence}/${name}.stderr.log" err)
    set(reason "Logging acceptance: PASS case=${name}")
    string(FIND "${out}" "${reason}" matched)
    if(NOT "${result}" STREQUAL "0" OR matched EQUAL -1)
        file(APPEND "${evidence}/results.csv" "${name},0,${result},FAIL\n")
        message(FATAL_ERROR "Logging case ${name}: expected exit=0 with '${reason}', got ${result}.\n${out}\n${err}\nEvidence retained: ${evidence}")
    endif()
    file(APPEND "${evidence}/results.csv" "${name},0,${result},PASS\n")
    message(STATUS "${name}: expected result verified (exit=${result})")
endfunction()

run_logging_case(roundtrip 45)
foreach(case file-failures producer-churn reader dll)
    run_logging_case("${case}" 60)
endforeach()
run_logging_case(rotation 120)
file(WRITE "${evidence}/complete.txt" "Logging acceptance: PASS\n")
message(STATUS "Logging acceptance: PASS; evidence=${evidence}")
