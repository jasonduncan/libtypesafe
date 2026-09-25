include(FetchContent)

FetchContent_Declare(Catch2
  URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.7.1.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP ON)
set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_INSTALL OFF CACHE BOOL "" FORCE)  # test-only: keep it out of the install tree
FetchContent_Declare(httplib
  URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.18.3.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP ON)
FetchContent_MakeAvailable(Catch2 httplib)

# Offline tests: no network except a local test server on 127.0.0.1.
add_executable(libtypesafe_tests
  test_headers.cpp
  test_questions.cpp
  test_wire.cpp
  test_config.cpp
  test_client.cpp
  test_curl.cpp)
target_link_libraries(libtypesafe_tests PRIVATE libtypesafe::libtypesafe Catch2::Catch2WithMain httplib::httplib)
target_include_directories(libtypesafe_tests PRIVATE ${PROJECT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(libtypesafe_tests PRIVATE cxx_std_17)

include(Catch)
catch_discover_tests(libtypesafe_tests)

# Live smoke test: calls the real API and spends tokens. Opt in with
#   TYPESAFE_API_KEY=... LIBTYPESAFE_LIVE=1 ctest -L live
add_executable(libtypesafe_live_tests test_live.cpp)
target_link_libraries(libtypesafe_live_tests PRIVATE libtypesafe::libtypesafe Catch2::Catch2WithMain)
add_test(NAME libtypesafe_live COMMAND libtypesafe_live_tests)
set_tests_properties(libtypesafe_live PROPERTIES LABELS live SKIP_RETURN_CODE 4)  # Catch2: all skipped

# Misuse that must not compile. check.cmake requires the build to fail *and* print the expected
# error, so a broken snippet can't pass by accident. Patterns cover clang, GCC, and MSVC wording.
set(_compile_fail_cases
  "wrong_answer_field|ScoreAnswer.*choice|choice.*ScoreAnswer"
  "enum_without_labels|specialize libtypesafe::choice_labels"
  "braced_labels|deleted function"
  "copy_pending|deleted")
foreach(entry ${_compile_fail_cases})
  string(FIND "${entry}" "|" _bar)
  string(SUBSTRING "${entry}" 0 ${_bar} _case)
  math(EXPR _rest "${_bar} + 1")
  string(SUBSTRING "${entry}" ${_rest} -1 _pattern)
  add_library(compile_fail_${_case} OBJECT EXCLUDE_FROM_ALL compile_fail/${_case}.cpp)
  target_link_libraries(compile_fail_${_case} PRIVATE libtypesafe_headers)
  add_test(NAME compile_fail.${_case}
    COMMAND ${CMAKE_COMMAND} -DBUILD_DIR=${PROJECT_BINARY_DIR} -DTARGET=compile_fail_${_case} -DCONFIG=$<CONFIG>
      "-DPATTERN=${_pattern}" -P ${CMAKE_CURRENT_SOURCE_DIR}/compile_fail/check.cmake)
endforeach()

# The package works from a separate project: installed (find_package) and vendored (add_subdirectory).
foreach(_mode install subdirectory)
  add_test(NAME consumer.${_mode}
    COMMAND ${CMAKE_COMMAND}
      -DMODE=${_mode}
      -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
      -DBUILD_DIR=${PROJECT_BINARY_DIR}
      -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/consumer-${_mode}
      -DCONFIG=$<CONFIG>
      "-DPREFIX_PATH=${CMAKE_PREFIX_PATH}"
      "-DTOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
      "-DGENERATOR=${CMAKE_GENERATOR}"
      -P ${CMAKE_CURRENT_SOURCE_DIR}/consumer/run.cmake)
  set_tests_properties(consumer.${_mode} PROPERTIES LABELS package TIMEOUT 600)
endforeach()
