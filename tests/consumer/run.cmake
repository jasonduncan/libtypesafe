# Builds and runs tests/consumer against libtypesafe.
#   cmake -DMODE=install|subdirectory -DSOURCE_DIR=... -DBUILD_DIR=... -DWORK_DIR=... [-DCONFIG=...]
#         [-DPREFIX_PATH=...] [-DTOOLCHAIN_FILE=...] [-DGENERATOR=...] -P run.cmake
# install: `cmake --install BUILD_DIR` into WORK_DIR/prefix, then find_package from there.
# subdirectory: add_subdirectory(SOURCE_DIR) from the consumer project.
if(NOT CONFIG)
  set(CONFIG Debug)
endif()
file(REMOVE_RECURSE "${WORK_DIR}")

function(run)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "failed (${rc}): ${ARGN}")
  endif()
endfunction()

set(prefix_path "${PREFIX_PATH}")
set(extra)
if(MODE STREQUAL "install")
  run(${CMAKE_COMMAND} --install "${BUILD_DIR}" --prefix "${WORK_DIR}/prefix" --config ${CONFIG})
  # Test-only dependencies must not leak into the package.
  file(GLOB_RECURSE leaked RELATIVE "${WORK_DIR}/prefix" "${WORK_DIR}/prefix/*httplib*" "${WORK_DIR}/prefix/*atch2*"
       "${WORK_DIR}/prefix/share/doc/*")
  if(leaked)
    message(FATAL_ERROR "install tree contains test-only files: ${leaked}")
  endif()
  list(PREPEND prefix_path "${WORK_DIR}/prefix")
elseif(MODE STREQUAL "subdirectory")
  list(APPEND extra "-DLIBTYPESAFE_SOURCE_DIR=${SOURCE_DIR}")
else()
  message(FATAL_ERROR "MODE must be install or subdirectory")
endif()
if(TOOLCHAIN_FILE)
  list(APPEND extra "-DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN_FILE}")
endif()
if(GENERATOR)
  list(APPEND extra -G "${GENERATOR}")
endif()
string(REPLACE ";" "\;" prefix_arg "${prefix_path}")

# Only the installed configuration exists, so multi-config generators (Visual Studio) must not
# generate the others: vcpkg's toolchain maps MinSizeRel/RelWithDebInfo to Release, which is absent.
run(${CMAKE_COMMAND} -S "${SOURCE_DIR}/tests/consumer" -B "${WORK_DIR}/build" ${extra}
    "-DCMAKE_PREFIX_PATH=${prefix_arg}" -DCMAKE_BUILD_TYPE=${CONFIG} -DCMAKE_CONFIGURATION_TYPES=${CONFIG})
run(${CMAKE_COMMAND} --build "${WORK_DIR}/build" --config ${CONFIG})
run(${CMAKE_CTEST_COMMAND} --test-dir "${WORK_DIR}/build" -C ${CONFIG} --output-on-failure)
