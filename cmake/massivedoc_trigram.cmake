target_sources(
  zevryon-massivedoc-core
  PRIVATE
    src/massivedoc_trigram_index.cpp
    src/massivedoc_trigram_store.cpp)

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

  add_executable(
    zevryon-massivedoc-trigram-store-tests
    tests/massivedoc_trigram_store_tests.cpp)
  target_link_libraries(
    zevryon-massivedoc-trigram-store-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-massivedoc-trigram-store-tests)
  add_test(
    NAME massivedoc-trigram-store-tests
    COMMAND zevryon-massivedoc-trigram-store-tests)

  add_executable(
    zevryon-massivedoc-trigram-find-tests
    tests/massivedoc_trigram_find_tests.cpp)
  target_link_libraries(
    zevryon-massivedoc-trigram-find-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-massivedoc-trigram-find-tests)
  add_test(
    NAME massivedoc-trigram-find-tests
    COMMAND zevryon-massivedoc-trigram-find-tests)

  add_executable(
    zevryon-massivedoc-search-cancellation-tests
    tests/massivedoc_search_cancellation_tests.cpp)
  target_link_libraries(
    zevryon-massivedoc-search-cancellation-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-massivedoc-search-cancellation-tests)
  add_test(
    NAME massivedoc-search-cancellation-tests
    COMMAND zevryon-massivedoc-search-cancellation-tests)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/unicode_search_normalization.cmake")
