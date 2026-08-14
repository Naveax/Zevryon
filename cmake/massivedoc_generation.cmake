find_package(Threads REQUIRED)

target_sources(
  zevryon-massivedoc-core
  PRIVATE
    src/massivedoc_block_cache.cpp
    src/massivedoc_cold_window.cpp
    src/massivedoc_generation.cpp
    src/massivedoc_generation_background.cpp
    src/massivedoc_generation_sync.cpp
    src/massivedoc_positional_io.cpp)

target_link_libraries(
  zevryon-massivedoc-core
  PUBLIC Threads::Threads)

if(BUILD_TESTING)
  add_executable(
    zevryon-massivedoc-block-cache-tests
    tests/massivedoc_block_cache_tests.cpp)
  target_link_libraries(
    zevryon-massivedoc-block-cache-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-massivedoc-block-cache-tests)
  add_test(
    NAME massivedoc-block-cache-tests
    COMMAND zevryon-massivedoc-block-cache-tests)

  add_executable(
    zevryon-massivedoc-cold-window-tests
    tests/massivedoc_cold_window_tests.cpp)
  target_link_libraries(
    zevryon-massivedoc-cold-window-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-massivedoc-cold-window-tests)
  add_test(
    NAME massivedoc-cold-window-tests
    COMMAND zevryon-massivedoc-cold-window-tests)

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
    zevryon-massivedoc-generation-background-tests
    tests/massivedoc_generation_background_tests.cpp)
  target_link_libraries(
    zevryon-massivedoc-generation-background-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-massivedoc-generation-background-tests)
  add_test(
    NAME massivedoc-generation-background-tests
    COMMAND zevryon-massivedoc-generation-background-tests)

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

  add_executable(
    zevryon-massivedoc-progressive-import-tests
    tests/massivedoc_progressive_import_tests.cpp)
  target_link_libraries(
    zevryon-massivedoc-progressive-import-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-massivedoc-progressive-import-tests)
  add_test(
    NAME massivedoc-progressive-import-tests
    COMMAND zevryon-massivedoc-progressive-import-tests)

  add_executable(
    zevryon-massivedoc-legacy-open-probe
    tests/massivedoc_legacy_open_probe.cpp)
  target_link_libraries(
    zevryon-massivedoc-legacy-open-probe
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-massivedoc-legacy-open-probe)

  add_executable(
    zevryon-massivedoc-cold-pss-probe
    tests/massivedoc_cold_pss_probe.cpp)
  target_link_libraries(
    zevryon-massivedoc-cold-pss-probe
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-massivedoc-cold-pss-probe)
endif()
