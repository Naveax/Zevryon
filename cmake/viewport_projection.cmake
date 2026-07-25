if(TARGET zevryon-line-box-layout)
  add_library(
    zevryon-viewport-projection STATIC
      src/viewport_projection.cpp)
  target_include_directories(zevryon-viewport-projection PUBLIC src)
  target_link_libraries(
    zevryon-viewport-projection
    PUBLIC zevryon-line-box-layout)
  zevryon_options(zevryon-viewport-projection)

  add_executable(
    zevryon-viewport-projection-benchmark
    src/viewport_projection_benchmark_main.cpp)
  target_link_libraries(
    zevryon-viewport-projection-benchmark
    PRIVATE zevryon-viewport-projection)
  zevryon_options(zevryon-viewport-projection-benchmark)

  if(BUILD_TESTING)
    add_executable(
      zevryon-viewport-projection-tests
      tests/viewport_projection_tests.cpp)
    target_link_libraries(
      zevryon-viewport-projection-tests
      PRIVATE zevryon-viewport-projection)
    zevryon_options(zevryon-viewport-projection-tests)
    add_test(
      NAME viewport-projection-tests
      COMMAND zevryon-viewport-projection-tests)

    add_executable(
      zevryon-viewport-hit-test-equivalence-tests
      tests/viewport_hit_test_equivalence_tests.cpp)
    target_link_libraries(
      zevryon-viewport-hit-test-equivalence-tests
      PRIVATE zevryon-viewport-projection)
    zevryon_options(zevryon-viewport-hit-test-equivalence-tests)
    add_test(
      NAME viewport-hit-test-equivalence-tests
      COMMAND zevryon-viewport-hit-test-equivalence-tests)
  endif()
endif()
