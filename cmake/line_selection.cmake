if(TARGET zevryon-harfbuzz-shaper)
  add_library(
    zevryon-line-selection STATIC
      src/line_selection.cpp)
  target_include_directories(zevryon-line-selection PUBLIC src)
  target_link_libraries(
    zevryon-line-selection
    PUBLIC
      zevryon-harfbuzz-shaper
      zevryon-unicode-line-break)
  zevryon_options(zevryon-line-selection)

  add_executable(
    zevryon-line-selection-benchmark
    src/line_selection_benchmark_main.cpp)
  target_link_libraries(
    zevryon-line-selection-benchmark
    PRIVATE zevryon-line-selection)
  zevryon_options(zevryon-line-selection-benchmark)

  if(BUILD_TESTING)
    add_executable(
      zevryon-line-selection-tests
      tests/line_selection_tests.cpp)
    target_link_libraries(
      zevryon-line-selection-tests
      PRIVATE zevryon-line-selection)
    zevryon_options(zevryon-line-selection-tests)
    add_test(
      NAME line-selection-tests
      COMMAND zevryon-line-selection-tests)

    add_executable(
      zevryon-line-selection-signed-advance-tests
      tests/line_selection_signed_advance_tests.cpp)
    target_link_libraries(
      zevryon-line-selection-signed-advance-tests
      PRIVATE zevryon-line-selection)
    zevryon_options(zevryon-line-selection-signed-advance-tests)
    add_test(
      NAME line-selection-signed-advance-tests
      COMMAND zevryon-line-selection-signed-advance-tests)
  endif()
endif()
