include_guard(GLOBAL)

target_sources(
  zevryon-massivedoc-core
  PRIVATE src/font_resource_sfnt.cpp)

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
endif()
