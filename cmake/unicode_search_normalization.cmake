target_sources(
  zevryon-massivedoc-core
  PRIVATE
    src/unicode_search_normalizer.cpp)

if(BUILD_TESTING)
  add_executable(
    zevryon-unicode-search-normalizer-tests
    tests/unicode_search_normalizer_tests.cpp)
  target_link_libraries(
    zevryon-unicode-search-normalizer-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-unicode-search-normalizer-tests)
  add_test(
    NAME unicode-search-normalizer-tests
    COMMAND zevryon-unicode-search-normalizer-tests)
endif()
