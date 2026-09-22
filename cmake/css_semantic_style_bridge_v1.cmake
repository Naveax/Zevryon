target_sources(
  zevryon-massivedoc-core
  PRIVATE
    src/css_semantic_style_bridge_v1.cpp
    src/css_inline_cascade_merge_v1.cpp)

if(BUILD_TESTING)
  add_executable(
    zevryon-css-semantic-style-bridge-v1-tests
    tests/css_semantic_style_bridge_v1_tests.cpp)
  target_link_libraries(
    zevryon-css-semantic-style-bridge-v1-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-css-semantic-style-bridge-v1-tests)
  add_test(
    NAME css-semantic-style-bridge-v1-foundation-tests
    COMMAND zevryon-css-semantic-style-bridge-v1-tests)

  add_executable(
    zevryon-css-inline-cascade-merge-v1-tests
    tests/css_inline_cascade_merge_v1_tests.cpp)
  target_link_libraries(
    zevryon-css-inline-cascade-merge-v1-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-css-inline-cascade-merge-v1-tests)
  add_test(
    NAME css-inline-cascade-merge-v1-foundation-tests
    COMMAND zevryon-css-inline-cascade-merge-v1-tests)
endif()
