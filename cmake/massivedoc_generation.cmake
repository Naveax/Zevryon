target_sources(
  zevryon-massivedoc-core
  PRIVATE
    src/massivedoc_generation.cpp
    src/massivedoc_positional_io.cpp)

if(BUILD_TESTING)
  add_executable(
    zevryon-massivedoc-generation-tests
    tests/massivedoc_generation_tests.cpp)
  target_link_libraries(
    zevryon-massivedoc-generation-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-massivedoc-generation-tests)
  add_test(
    NAME massivedoc-generation-tests
    COMMAND zevryon-massivedoc-generation-tests)

  add_executable(
    zevryon-massivedoc-generation-runtime-tests
    tests/massivedoc_generation_runtime_tests.cpp)
  target_link_libraries(
    zevryon-massivedoc-generation-runtime-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-massivedoc-generation-runtime-tests)
  add_test(
    NAME massivedoc-generation-runtime-tests
    COMMAND zevryon-massivedoc-generation-runtime-tests)

  add_executable(
    zevryon-massivedoc-generation-compaction-tests
    tests/massivedoc_generation_compaction_tests.cpp)
  target_link_libraries(
    zevryon-massivedoc-generation-compaction-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-massivedoc-generation-compaction-tests)
  add_test(
    NAME massivedoc-generation-compaction-tests
    COMMAND zevryon-massivedoc-generation-compaction-tests)

  add_executable(
    zevryon-massivedoc-positional-io-tests
    tests/massivedoc_positional_io_tests.cpp)
  target_link_libraries(
    zevryon-massivedoc-positional-io-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-massivedoc-positional-io-tests)
  add_test(
    NAME massivedoc-positional-io-tests
    COMMAND zevryon-massivedoc-positional-io-tests)

  add_executable(
    zevryon-massivedoc-positional-store-tests
    tests/massivedoc_positional_store_tests.cpp)
  target_link_libraries(
    zevryon-massivedoc-positional-store-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-massivedoc-positional-store-tests)
  add_test(
    NAME massivedoc-positional-store-tests
    COMMAND zevryon-massivedoc-positional-store-tests)

  add_executable(
    zevryon-massivedoc-positional-search-tests
    tests/massivedoc_positional_search_tests.cpp)
  target_link_libraries(
    zevryon-massivedoc-positional-search-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-massivedoc-positional-search-tests)
  add_test(
    NAME massivedoc-positional-search-tests
    COMMAND zevryon-massivedoc-positional-search-tests)
endif()
