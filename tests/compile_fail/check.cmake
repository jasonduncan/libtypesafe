# Passes only if building TARGET fails with output matching PATTERN.
execute_process(
  COMMAND ${CMAKE_COMMAND} --build "${BUILD_DIR}" --target "${TARGET}" --config "${CONFIG}"
  RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
if(rc EQUAL 0)
  message(FATAL_ERROR "${TARGET} compiled, but it must not.")
endif()
if(NOT out MATCHES "${PATTERN}")
  message(FATAL_ERROR "${TARGET} failed for another reason (expected /${PATTERN}/):\n${out}")
endif()
message(STATUS "${TARGET} failed as expected.")
