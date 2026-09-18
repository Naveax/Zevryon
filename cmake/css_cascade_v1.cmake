target_sources(
  zevryon-massivedoc-core
  PRIVATE
    src/css_cascade_v1.cpp)

if(BUILD_TESTING)
  add_executable(
    zevryon-css-cascade-v1-tests
    tests/css_cascade_v1_tests.cpp)
  target_link_libraries(
    zevryon-css-cascade-v1-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-css-cascade-v1-tests)
  add_test(
    NAME css-cascade-v1-foundation-tests
    COMMAND zevryon-css-cascade-v1-tests)
endif()
