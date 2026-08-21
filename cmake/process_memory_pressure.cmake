target_sources(
  zevryon-massivedoc-core
  PRIVATE
    src/zenith_process_memory_pressure.cpp
    src/zenith_process_memory_pressure_apply.cpp
    src/zenith_process_memory_sampler.cpp)

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

  add_executable(
    zevryon-process-memory-sampler-tests
    tests/zenith_process_memory_sampler_tests.cpp)
  target_link_libraries(
    zevryon-process-memory-sampler-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-process-memory-sampler-tests)
  add_test(
    NAME process-memory-sampler-tests
    COMMAND zevryon-process-memory-sampler-tests)

  add_executable(
    zevryon-process-memory-runtime-integration-tests
    tests/zenith_process_memory_runtime_integration_tests.cpp)
  target_link_libraries(
    zevryon-process-memory-runtime-integration-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-process-memory-runtime-integration-tests)
  add_test(
    NAME process-memory-runtime-integration-tests
    COMMAND zevryon-process-memory-runtime-integration-tests)
endif()
