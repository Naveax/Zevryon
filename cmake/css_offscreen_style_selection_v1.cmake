target_sources(
  zevryon-massivedoc-core
  PRIVATE
    src/css_offscreen_style_selection_v1.cpp)

if(BUILD_TESTING)
  add_executable(
    zevryon-css-offscreen-style-selection-v1-tests
    tests/css_offscreen_style_selection_v1_tests.cpp)
  target_link_libraries(
    zevryon-css-offscreen-style-selection-v1-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-css-offscreen-style-selection-v1-tests)
  add_test(
    NAME css-offscreen-style-selection-v1-foundation-tests
    COMMAND zevryon-css-offscreen-style-selection-v1-tests)
endif()
