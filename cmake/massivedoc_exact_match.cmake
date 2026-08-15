target_sources(
  zevryon-massivedoc-core
  PRIVATE
    src/massivedoc_exact_match.cpp)

if(BUILD_TESTING)
  add_executable(
    zevryon-massivedoc-exact-match-tests
    tests/massivedoc_exact_match_tests.cpp)
  target_link_libraries(
    zevryon-massivedoc-exact-match-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-massivedoc-exact-match-tests)
  add_test(
    NAME massivedoc-exact-match-tests
    COMMAND zevryon-massivedoc-exact-match-tests)
endif()
