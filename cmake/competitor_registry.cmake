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
      NAME competitor-benchmark-evidence-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/browser_competitor_benchmark_evidence_tests.py")
    add_test(
      NAME competitor-process-scope-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/browser_competitor_process_scope_tests.py")
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
      NAME competitor-benchmark-evidence-tests
      COMMAND "${CMAKE_COMMAND}" -E false)
    add_test(
      NAME competitor-process-scope-tests
      COMMAND "${CMAKE_COMMAND}" -E false)
  endif()
endif()
