target_sources(
  zevryon-massivedoc-core
  PRIVATE
    src/css_selector_v1.cpp
    src/css_selector_invalidation_v1.cpp)

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

  add_executable(
    zevryon-css-selector-invalidation-v1-tests
    tests/css_selector_invalidation_v1_tests.cpp)
  target_link_libraries(
    zevryon-css-selector-invalidation-v1-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-css-selector-invalidation-v1-tests)
  add_test(
    NAME css-selector-invalidation-v1-foundation-tests
    COMMAND zevryon-css-selector-invalidation-v1-tests)

  add_executable(
    zevryon-z3-selector-invalidation-authority-tests
    tests/z3_selector_invalidation_authority_tests.cpp)
  target_link_libraries(
    zevryon-z3-selector-invalidation-authority-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-z3-selector-invalidation-authority-tests)
  add_test(
    NAME z3-selector-invalidation-authority-v1
    COMMAND zevryon-z3-selector-invalidation-authority-tests)
endif()
