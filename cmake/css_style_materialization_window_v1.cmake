target_sources(
  zevryon-massivedoc-core
  PRIVATE
    src/css_style_materialization_window_v1.cpp)

if(BUILD_TESTING)
  add_executable(
    zevryon-css-style-materialization-window-v1-tests
    tests/css_style_materialization_window_v1_tests.cpp)
  target_link_libraries(
    zevryon-css-style-materialization-window-v1-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-css-style-materialization-window-v1-tests)
  add_test(
    NAME css-style-materialization-window-v1-foundation-tests
    COMMAND zevryon-css-style-materialization-window-v1-tests)
endif()
