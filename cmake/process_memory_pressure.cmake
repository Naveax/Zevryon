target_sources(
  zevryon-massivedoc-core
  PRIVATE
    src/zenith_process_memory_pressure.cpp
    src/zenith_process_memory_pressure_apply.cpp
    src/zenith_process_memory_sampler.cpp
    src/zenith_process_runtime_services.cpp)

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
    zevryon-process-runtime-services-tests
    tests/zenith_process_runtime_services_tests.cpp)
  target_link_libraries(
    zevryon-process-runtime-services-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-process-runtime-services-tests)
  add_test(
    NAME process-runtime-services-tests
    COMMAND zevryon-process-runtime-services-tests)

  add_executable(
    zevryon-dormant-registry-scale-tests
    tests/zenith_process_dormant_registry_scale_tests.cpp)
  target_link_libraries(
    zevryon-dormant-registry-scale-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-dormant-registry-scale-tests)
  add_test(
    NAME dormant-registry-scale-tests
    COMMAND zevryon-dormant-registry-scale-tests)

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

  add_executable(
    zevryon-runtime-generation-retirement-tests
    tests/zenith_process_runtime_generation_tests.cpp)
  target_link_libraries(
    zevryon-runtime-generation-retirement-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-runtime-generation-retirement-tests)
  add_test(
    NAME runtime-generation-retirement-tests
    COMMAND zevryon-runtime-generation-retirement-tests)
endif()
