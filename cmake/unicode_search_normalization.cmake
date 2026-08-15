target_sources(
  zevryon-massivedoc-core
  PRIVATE
    src/unicode_search_normalizer.cpp
    src/massivedoc_unicode_matcher.cpp
    src/massivedoc_unicode_search_runtime.cpp)

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

  add_executable(
    zevryon-massivedoc-unicode-matcher-tests
    tests/massivedoc_unicode_matcher_tests.cpp)
  target_link_libraries(
    zevryon-massivedoc-unicode-matcher-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-massivedoc-unicode-matcher-tests)
  add_test(
    NAME massivedoc-unicode-matcher-tests
    COMMAND zevryon-massivedoc-unicode-matcher-tests)

  add_executable(
    zevryon-massivedoc-unicode-runtime-tests
    tests/massivedoc_unicode_search_runtime_tests.cpp)
  target_link_libraries(
    zevryon-massivedoc-unicode-runtime-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-massivedoc-unicode-runtime-tests)
  add_test(
    NAME massivedoc-unicode-runtime-tests
    COMMAND zevryon-massivedoc-unicode-runtime-tests)

  add_executable(
    zevryon-massivedoc-unicode-search-tests
    tests/massivedoc_unicode_search_tests.cpp)
  target_link_libraries(
    zevryon-massivedoc-unicode-search-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-massivedoc-unicode-search-tests)
  add_test(
    NAME massivedoc-unicode-search-tests
    COMMAND zevryon-massivedoc-unicode-search-tests)

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

include("${CMAKE_CURRENT_LIST_DIR}/full_document_operations.cmake")
