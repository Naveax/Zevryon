include_guard(GLOBAL)

find_package(Threads REQUIRED)

target_sources(
  zevryon-massivedoc-core
  PRIVATE
    src/font_resource_sfnt.cpp
    src/font_resource_integrity.cpp
    src/font_content_identity.cpp
    src/font_load_locator.cpp
    src/verified_font_resource.cpp
    src/verified_font_resource_cache.cpp
    src/verified_font_resource_cache_identity.cpp
    src/font_file_loader.cpp)
target_link_libraries(
  zevryon-massivedoc-core
  PUBLIC Threads::Threads)

add_executable(
  zevryon-font-content-identity-benchmark
  src/font_content_identity_benchmark_main.cpp)
target_link_libraries(
  zevryon-font-content-identity-benchmark
  PRIVATE zevryon-massivedoc-core)
zevryon_options(zevryon-font-content-identity-benchmark)

add_executable(
  zevryon-font-load-locator-benchmark
  src/font_load_locator_benchmark_main.cpp)
target_link_libraries(
  zevryon-font-load-locator-benchmark
  PRIVATE zevryon-massivedoc-core)
zevryon_options(zevryon-font-load-locator-benchmark)

if(BUILD_TESTING)
  add_executable(
    zevryon-font-resource-sfnt-tests
    tests/font_resource_sfnt_tests.cpp)
  target_link_libraries(
    zevryon-font-resource-sfnt-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-font-resource-sfnt-tests)
  add_test(
    NAME font-resource-sfnt-tests
    COMMAND zevryon-font-resource-sfnt-tests)

  add_executable(
    zevryon-font-resource-integrity-tests
    tests/font_resource_integrity_tests.cpp)
  target_link_libraries(
    zevryon-font-resource-integrity-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-font-resource-integrity-tests)
  add_test(
    NAME font-resource-integrity-tests
    COMMAND zevryon-font-resource-integrity-tests)

  add_executable(
    zevryon-font-content-identity-tests
    tests/font_content_identity_tests.cpp)
  target_link_libraries(
    zevryon-font-content-identity-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-font-content-identity-tests)
  add_test(
    NAME font-content-identity-tests
    COMMAND zevryon-font-content-identity-tests)

  add_executable(
    zevryon-font-load-locator-tests
    tests/font_load_locator_tests.cpp)
  target_link_libraries(
    zevryon-font-load-locator-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-font-load-locator-tests)
  add_test(
    NAME font-load-locator-tests
    COMMAND zevryon-font-load-locator-tests)

  add_executable(
    zevryon-font-file-loader-tests
    tests/font_file_loader_tests.cpp)
  target_link_libraries(
    zevryon-font-file-loader-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-font-file-loader-tests)
  add_test(
    NAME font-file-loader-tests
    COMMAND zevryon-font-file-loader-tests)

  add_executable(
    zevryon-verified-font-resource-tests
    tests/verified_font_resource_tests.cpp)
  target_link_libraries(
    zevryon-verified-font-resource-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-verified-font-resource-tests)
  add_test(
    NAME verified-font-resource-tests
    COMMAND zevryon-verified-font-resource-tests)

  add_executable(
    zevryon-verified-font-resource-cache-tests
    tests/verified_font_resource_cache_tests.cpp)
  target_link_libraries(
    zevryon-verified-font-resource-cache-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-verified-font-resource-cache-tests)
  add_test(
    NAME verified-font-resource-cache-tests
    COMMAND zevryon-verified-font-resource-cache-tests)
endif()
