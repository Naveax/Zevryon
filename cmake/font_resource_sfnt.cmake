include_guard(GLOBAL)

target_sources(
  zevryon-massivedoc-core
  PRIVATE
    src/font_resource_sfnt.cpp
    src/font_resource_integrity.cpp)

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
endif()
