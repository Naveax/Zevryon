if(BUILD_TESTING)
  add_executable(
    zevryon-m8-storage-crash-cut-tests
    tests/m8_storage_crash_cut_tests.cpp)
  target_link_libraries(
    zevryon-m8-storage-crash-cut-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-m8-storage-crash-cut-tests)
  add_test(
    NAME m8-storage-crash-cut-tests
    COMMAND zevryon-m8-storage-crash-cut-tests)

  add_executable(
    zevryon-m8-mixed-mutation-probe
    tests/m8_mixed_mutation_probe.cpp)
  target_link_libraries(
    zevryon-m8-mixed-mutation-probe
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-m8-mixed-mutation-probe)

  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(
      NAME m8-mixed-mutation-authority-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/m8_mixed_mutation_probe_tests.py"
        --probe "$<TARGET_FILE:zevryon-m8-mixed-mutation-probe>"
        --work-dir "${CMAKE_CURRENT_BINARY_DIR}/m8-mixed-mutation-smoke")
  endif()
endif()
