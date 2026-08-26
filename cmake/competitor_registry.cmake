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
  else()
    add_test(
      NAME competitor-registry-tests
      COMMAND "${CMAKE_COMMAND}" -E false)
    add_test(
      NAME competitor-playwright-adapter-tests
      COMMAND "${CMAKE_COMMAND}" -E false)
  endif()
endif()
