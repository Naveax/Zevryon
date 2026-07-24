target_sources(
  zevryon-massivedoc-core
  PRIVATE src/opentype_directory.cpp)

if(BUILD_TESTING)
  add_executable(
    zevryon-opentype-directory-tests
    tests/opentype_directory_tests.cpp)
  target_link_libraries(
    zevryon-opentype-directory-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-opentype-directory-tests)
  add_test(
    NAME opentype-directory-tests
    COMMAND zevryon-opentype-directory-tests)
endif()
