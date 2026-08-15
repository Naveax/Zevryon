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

  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/unicode_search_normalization_data.generated.hpp")
    add_executable(
      zevryon-unicode-search-production-table-tests
      tests/unicode_search_production_table_tests.cpp)
    target_link_libraries(
      zevryon-unicode-search-production-table-tests
      PRIVATE zevryon-massivedoc-core)
    zevryon_options(zevryon-unicode-search-production-table-tests)
    add_test(
      NAME unicode-search-production-table-tests
      COMMAND zevryon-unicode-search-production-table-tests)

    add_executable(
      zevryon-unicode-search-normalization-conformance
      tests/unicode_search_normalization_conformance.cpp)
    target_link_libraries(
      zevryon-unicode-search-normalization-conformance
      PRIVATE zevryon-massivedoc-core)
    zevryon_options(zevryon-unicode-search-normalization-conformance)
  endif()
endif()
