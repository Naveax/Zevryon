target_sources(
  zevryon-massivedoc-core
  PRIVATE src/massivedoc_benchmark_session.cpp)

add_executable(
  zevryon-massivedoc-benchmark-session
  src/massivedoc_benchmark_session_main.cpp)
target_link_libraries(
  zevryon-massivedoc-benchmark-session
  PRIVATE zevryon-massivedoc-core)
zevryon_options(zevryon-massivedoc-benchmark-session)

if(BUILD_TESTING)
  add_executable(
    zevryon-massivedoc-benchmark-session-tests
    tests/massivedoc_benchmark_session_tests.cpp)
  target_link_libraries(
    zevryon-massivedoc-benchmark-session-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-massivedoc-benchmark-session-tests)
  add_test(
    NAME massivedoc-benchmark-session-tests
    COMMAND zevryon-massivedoc-benchmark-session-tests)

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
      NAME m7-synthetic-corpus-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/m7_synthetic_corpus_tests.py")
    add_test(
      NAME competitor-process-scope-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/browser_competitor_process_scope_tests.py")
    add_test(
      NAME competitor-servo-adapter-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/browser_competitor_servo_tests.py")
    add_test(
      NAME competitor-ladybird-adapter-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/browser_competitor_ladybird_tests.py")
    add_test(
      NAME competitor-webdriver-transport-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/browser_competitor_webdriver_tests.py")
    add_test(
      NAME competitor-webdriver-scenario-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/browser_competitor_webdriver_scenario_tests.py")
    add_test(
      NAME competitor-webdriver-runtime-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/browser_competitor_webdriver_runtime_tests.py")
    add_test(
      NAME competitor-scenario-contract-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/browser_competitor_scenario_contract_tests.py")
    add_test(
      NAME competitor-case-executor-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/browser_competitor_case_executor_tests.py")
    add_test(
      NAME competitor-benchmark-runner-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/browser_competitor_benchmark_runner_tests.py")
    add_test(
      NAME competitor-normalized-core-evidence-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/browser_competitor_normalized_core_evidence_tests.py")
    add_test(
      NAME competitor-canonical-full-set-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/browser_competitor_canonical_full_set_tests.py")
  else()
    foreach(_test_name IN ITEMS
        competitor-registry-tests
        competitor-playwright-adapter-tests
        competitor-benchmark-planner-tests
        competitor-benchmark-evidence-tests
        m7-synthetic-corpus-tests
        competitor-process-scope-tests
        competitor-servo-adapter-tests
        competitor-ladybird-adapter-tests
        competitor-webdriver-transport-tests
        competitor-webdriver-scenario-tests
        competitor-webdriver-runtime-tests
        competitor-scenario-contract-tests
        competitor-case-executor-tests
        competitor-benchmark-runner-tests
        competitor-normalized-core-evidence-tests
        competitor-canonical-full-set-tests)
      add_test(NAME "${_test_name}" COMMAND "${CMAKE_COMMAND}" -E false)
    endforeach()
  endif()
endif()
