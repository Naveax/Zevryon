target_sources(
  zevryon-massivedoc-core
  PRIVATE
    src/z4_layout_style_invalidation_v1.cpp)

if(BUILD_TESTING)
  add_executable(
    zevryon-z4-layout-style-invalidation-v1-tests
    tests/z4_layout_style_invalidation_v1_tests.cpp)
  target_link_libraries(
    zevryon-z4-layout-style-invalidation-v1-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-z4-layout-style-invalidation-v1-tests)
  add_test(
    NAME z4-layout-style-invalidation-v1-foundation-tests
    COMMAND zevryon-z4-layout-style-invalidation-v1-tests)
endif()
