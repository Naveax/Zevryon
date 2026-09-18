target_sources(
  zevryon-massivedoc-core
  PRIVATE
    src/css_parser_v1.cpp)

if(BUILD_TESTING)
  add_executable(
    zevryon-css-parser-v1-tests
    tests/css_parser_v1_tests.cpp)
  target_link_libraries(
    zevryon-css-parser-v1-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-css-parser-v1-tests)
  add_test(
    NAME css-parser-v1-foundation-tests
    COMMAND zevryon-css-parser-v1-tests)
endif()
