if(BUILD_TESTING)
  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(
      NAME competitor-registry-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/browser_competitor_registry_tests.py")
    add_test(
      NAME competitor-playwright-adapter-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/browser_competitor_playwright_tests.py")
    add_test(
      NAME competitor-benchmark-planner-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/browser_competitor_benchmark_plan_tests.py")
    add_test(
      NAME competitor-evidence-context-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/browser_competitor_evidence_context_tests.py")
    add_test(
      NAME competitor-benchmark-executor-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/browser_competitor_benchmark_executor_tests.py")
    add_test(
      NAME competitor-benchmark-matrix-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/browser_competitor_benchmark_matrix_tests.py")
  else()
    add_test(
      NAME competitor-registry-tests
      COMMAND "${CMAKE_COMMAND}" -E false)
    add_test(
      NAME competitor-playwright-adapter-tests
      COMMAND "${CMAKE_COMMAND}" -E false)
    add_test(
      NAME competitor-benchmark-planner-tests
      COMMAND "${CMAKE_COMMAND}" -E false)
    add_test(
      NAME competitor-evidence-context-tests
      COMMAND "${CMAKE_COMMAND}" -E false)
    add_test(
      NAME competitor-benchmark-executor-tests
      COMMAND "${CMAKE_COMMAND}" -E false)
    add_test(
      NAME competitor-benchmark-matrix-tests
      COMMAND "${CMAKE_COMMAND}" -E false)
  endif()
endif()
