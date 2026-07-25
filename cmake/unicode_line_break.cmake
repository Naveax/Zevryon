add_library(
  zevryon-unicode-line-break STATIC
    src/unicode_line_break_data.cpp
    src/line_break_opportunity.cpp)
target_include_directories(zevryon-unicode-line-break PUBLIC src)
target_link_libraries(
  zevryon-unicode-line-break
  PUBLIC zevryon-massivedoc-core)
zevryon_options(zevryon-unicode-line-break)

add_executable(
  zevryon-line-break-opportunity-benchmark
  src/line_break_benchmark_main.cpp)
target_link_libraries(
  zevryon-line-break-opportunity-benchmark
  PRIVATE zevryon-unicode-line-break)
zevryon_options(zevryon-line-break-opportunity-benchmark)

add_executable(
  zevryon-line-break-conformance
  src/line_break_conformance_main.cpp)
target_link_libraries(
  zevryon-line-break-conformance
  PRIVATE zevryon-unicode-line-break)
zevryon_options(zevryon-line-break-conformance)

if(BUILD_TESTING)
  add_executable(
    zevryon-line-break-opportunity-tests
    tests/line_break_opportunity_tests.cpp)
  target_link_libraries(
    zevryon-line-break-opportunity-tests
    PRIVATE zevryon-unicode-line-break)
  zevryon_options(zevryon-line-break-opportunity-tests)
  add_test(
    NAME line-break-opportunity-tests
    COMMAND zevryon-line-break-opportunity-tests)
endif()
