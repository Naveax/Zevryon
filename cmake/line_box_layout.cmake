if(TARGET zevryon-line-fragment-layout)
  add_library(
    zevryon-line-box-layout STATIC
      src/font_line_metrics.cpp
      src/line_box_layout.cpp)
  target_include_directories(zevryon-line-box-layout PUBLIC src)
  target_link_libraries(
    zevryon-line-box-layout
    PUBLIC zevryon-line-fragment-layout)
  zevryon_options(zevryon-line-box-layout)

  add_executable(
    zevryon-line-box-layout-benchmark
    src/line_box_layout_benchmark_main.cpp)
  target_link_libraries(
    zevryon-line-box-layout-benchmark
    PRIVATE zevryon-line-box-layout)
  zevryon_options(zevryon-line-box-layout-benchmark)

  if(BUILD_TESTING)
    add_executable(
      zevryon-font-line-metrics-tests
      tests/font_line_metrics_tests.cpp)
    target_link_libraries(
      zevryon-font-line-metrics-tests
      PRIVATE zevryon-line-box-layout)
    zevryon_options(zevryon-font-line-metrics-tests)
    add_test(
      NAME font-line-metrics-tests
      COMMAND zevryon-font-line-metrics-tests)

    add_executable(
      zevryon-line-box-layout-tests
      tests/line_box_layout_tests.cpp)
    target_link_libraries(
      zevryon-line-box-layout-tests
      PRIVATE zevryon-line-box-layout)
    zevryon_options(zevryon-line-box-layout-tests)
    add_test(
      NAME line-box-layout-tests
      COMMAND zevryon-line-box-layout-tests)

    add_executable(
      zevryon-line-box-layout-equivalence-tests
      tests/line_box_layout_equivalence_tests.cpp)
    target_link_libraries(
      zevryon-line-box-layout-equivalence-tests
      PRIVATE zevryon-line-box-layout)
    zevryon_options(zevryon-line-box-layout-equivalence-tests)
    add_test(
      NAME line-box-layout-equivalence-tests
      COMMAND zevryon-line-box-layout-equivalence-tests)
  endif()
endif()
