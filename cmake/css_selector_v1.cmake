target_sources(
  zevryon-massivedoc-core
  PRIVATE
    src/css_selector_v1.cpp)

if(BUILD_TESTING)
  add_executable(
    zevryon-css-selector-v1-tests
    tests/css_selector_v1_tests.cpp)
  target_link_libraries(
    zevryon-css-selector-v1-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-css-selector-v1-tests)
  add_test(
    NAME css-selector-v1-foundation-tests
    COMMAND zevryon-css-selector-v1-tests)
endif()
