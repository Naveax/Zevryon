if(TARGET zevryon-line-selection)
  add_library(
    zevryon-line-fragment-layout STATIC
      src/line_fragment_layout.cpp)
  target_include_directories(zevryon-line-fragment-layout PUBLIC src)
  target_link_libraries(
    zevryon-line-fragment-layout
    PUBLIC
      zevryon-line-selection
      zevryon-massivedoc-core)
  zevryon_options(zevryon-line-fragment-layout)

  add_executable(
    zevryon-line-fragment-layout-benchmark
    src/line_fragment_layout_benchmark_main.cpp)
  target_link_libraries(
    zevryon-line-fragment-layout-benchmark
    PRIVATE zevryon-line-fragment-layout)
  zevryon_options(zevryon-line-fragment-layout-benchmark)

  if(BUILD_TESTING)
    add_executable(
      zevryon-line-fragment-layout-tests
      tests/line_fragment_layout_tests.cpp)
    target_link_libraries(
      zevryon-line-fragment-layout-tests
      PRIVATE zevryon-line-fragment-layout)
    zevryon_options(zevryon-line-fragment-layout-tests)
    add_test(
      NAME line-fragment-layout-tests
      COMMAND zevryon-line-fragment-layout-tests)
  endif()
endif()
