include_guard(GLOBAL)
include(FetchContent)
file(READ "${CMAKE_CURRENT_LIST_DIR}/dependencies-lock.json" _quill_dependencies)
foreach(_field url sha256 filename)
    string(JSON _quill_${_field} GET "${_quill_dependencies}" quill ${_field})
endforeach()
foreach(_option BUILD_EXAMPLES BUILD_MODULE BUILD_TESTS BUILD_BENCHMARKS BUILD_FUZZING ENABLE_INSTALL DOCS_GEN)
    set(QUILL_${_option} OFF CACHE BOOL "" FORCE)
endforeach()
set(QUILL_NO_EXCEPTIONS OFF CACHE BOOL "" FORCE)
set(QUILL_DISABLE_NON_PREFIXED_MACROS ON CACHE BOOL "" FORCE)
FetchContent_Declare(atelier_quill
    URL "${_quill_url}" URL_HASH "SHA256=${_quill_sha256}"
    DOWNLOAD_NAME "${_quill_filename}"
    DOWNLOAD_DIR "${PROJECT_SOURCE_DIR}/.tools/downloads" EXCLUDE_FROM_ALL SYSTEM)
FetchContent_MakeAvailable(atelier_quill)
configure_file("${atelier_quill_SOURCE_DIR}/LICENSE" "${CMAKE_BINARY_DIR}/LICENSE.quill.txt" COPYONLY)
# The bundled fmt license (including its optional object-code exception) is the
# opening comment, not a separately shipped LICENSE file. Preserve it verbatim.
file(READ "${atelier_quill_SOURCE_DIR}/include/quill/bundled/fmt/format.h" _quill_fmt_header)
string(FIND "${_quill_fmt_header}" "*/" _quill_fmt_license_end)
if(_quill_fmt_license_end LESS 0)
    message(FATAL_ERROR "Pinned Quill bundled fmt license is missing")
endif()
math(EXPR _quill_fmt_license_length "${_quill_fmt_license_end} + 2")
string(SUBSTRING "${_quill_fmt_header}" 0 ${_quill_fmt_license_length} _quill_fmt_license)
file(WRITE "${CMAKE_BINARY_DIR}/LICENSE.quill-fmt.txt" "${_quill_fmt_license}\n")
