target_sources(
  zevryon-massivedoc-core
  PRIVATE src/zenith_process_memory_pressure.cpp)

if(WIN32)
  target_link_libraries(zevryon-massivedoc-core PRIVATE psapi)
endif()

if(BUILD_TESTING)
  add_executable(
    zevryon-process-memory-pressure-tests
    tests/zenith_process_memory_pressure_tests.cpp)
  target_link_libraries(
    zevryon-process-memory-pressure-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-process-memory-pressure-tests)
  add_test(
    NAME process-memory-pressure-tests
    COMMAND zevryon-process-memory-pressure-tests)
endif()
