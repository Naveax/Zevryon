target_sources(
  zevryon-massivedoc-core
  PRIVATE src/massivedoc_trigram_index.cpp)

if(BUILD_TESTING)
  add_executable(
    zevryon-massivedoc-trigram-index-tests
    tests/massivedoc_trigram_index_tests.cpp)
  target_link_libraries(
    zevryon-massivedoc-trigram-index-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-massivedoc-trigram-index-tests)
  add_test(
    NAME massivedoc-trigram-index-tests
    COMMAND zevryon-massivedoc-trigram-index-tests)
endif()
