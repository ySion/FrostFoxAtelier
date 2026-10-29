cmake_minimum_required(VERSION 4.2)

foreach(required PROGRAM PRODUCER_DLL BUILD_DIRECTORY SOURCE_DIRECTORY EVIDENCE_ROOT MODE)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "Logging acceptance requires ${required}.")
    endif()
endforeach()
if(NOT EXISTS "${PROGRAM}" OR NOT EXISTS "${PRODUCER_DLL}")
    message(FATAL_ERROR "Build the formal logging consumer and producer DLL first.")
endif()
if(NOT MODE STREQUAL "contracts" AND NOT MODE STREQUAL "pressure")
    message(FATAL_ERROR "Unknown logging acceptance mode: ${MODE}")
endif()

string(TIMESTAMP stamp "%Y%m%d-%H%M%S" UTC)
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef nonce)
set(evidence "${EVIDENCE_ROOT}/${stamp}-${nonce}-${MODE}")
file(MAKE_DIRECTORY "${evidence}")
file(WRITE "${evidence}/results.csv" "Case,ExpectedExit,ActualExit,Result\n")
message(STATUS "Logging acceptance evidence: ${evidence}")

function(run_logging_case name expected reason timeout)
    # One successful Run per child process; ordinary exceptions, loader failures
    # and crashes cannot satisfy the expected-warning exit/reason pair.
    execute_process(COMMAND "${PROGRAM}" "${name}" "${evidence}/${name}" "${PRODUCER_DLL}"
        WORKING_DIRECTORY "${BUILD_DIRECTORY}"
        TIMEOUT ${timeout} RESULT_VARIABLE result
        OUTPUT_FILE "${evidence}/${name}.stdout.log" ERROR_FILE "${evidence}/${name}.stderr.log")
    file(READ "${evidence}/${name}.stdout.log" out)
    file(READ "${evidence}/${name}.stderr.log" err)
    string(FIND "${out}" "${reason}" matched)
    if(NOT "${result}" STREQUAL "${expected}" OR matched EQUAL -1)
        file(APPEND "${evidence}/results.csv" "${name},${expected},${result},FAIL\n")
        message(FATAL_ERROR "Logging case ${name}: expected exit=${expected} with '${reason}', got ${result}.\n${out}\n${err}\nEvidence retained: ${evidence}")
    endif()
    file(APPEND "${evidence}/results.csv" "${name},${expected},${result},PASS\n")
    message(STATUS "${name}: expected result verified (exit=${result})")
endfunction()

if(MODE STREQUAL "pressure")
    run_logging_case(pressure 0 "Logging acceptance: PASS case=pressure reason=queue-full-observed-and-accounted" 90)
else()
    # First prove the vertical public path before running any counterexample.
    run_logging_case(roundtrip 0 "Logging acceptance: PASS case=roundtrip" 45)
    foreach(case boundaries file-failures threads producer-churn reader dll)
        run_logging_case("${case}" 0 "Logging acceptance: PASS case=${case}" 60)
    endforeach()
    run_logging_case(rotation 0 "Logging acceptance: PASS case=rotation" 120)
    run_logging_case(unexpected-warning 23
        "Logging acceptance: REJECT case=unexpected-warning reason=unexpected-warning category=acceptance.cpu event=unexpected.warning phase=validation context=71 count=1" 45)
    include("${SOURCE_DIRECTORY}/cmake/run-logging-format-acceptance.cmake")
endif()
file(WRITE "${evidence}/complete.txt" "Logging acceptance: PASS mode=${MODE}\n")
message(STATUS "Logging acceptance: PASS mode=${MODE}; evidence=${evidence}")
