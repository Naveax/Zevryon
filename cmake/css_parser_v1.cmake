target_sources(
  zevryon-massivedoc-core
  PRIVATE
    src/css_parser_v1.cpp)

if(BUILD_TESTING)
  add_executable(
    zevryon-css-parser-v1-tests
    tests/css_parser_v1_tests.cpp)
  target_link_libraries(
    zevryon-css-parser-v1-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-css-parser-v1-tests)
  add_test(
    NAME css-parser-v1-foundation-tests
    COMMAND zevryon-css-parser-v1-tests)

  add_executable(
    zevryon-css-parser-at-rule-recovery-v1-tests
    tests/css_parser_at_rule_recovery_v1_tests.cpp)
  target_link_libraries(
    zevryon-css-parser-at-rule-recovery-v1-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-css-parser-at-rule-recovery-v1-tests)
  add_test(
    NAME css-parser-at-rule-recovery-v1-tests
    COMMAND zevryon-css-parser-at-rule-recovery-v1-tests)

  add_executable(
    zevryon-css-declaration-list-v1-tests
    tests/css_declaration_list_v1_tests.cpp)
  target_link_libraries(
    zevryon-css-declaration-list-v1-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-css-declaration-list-v1-tests)
  add_test(
    NAME css-declaration-list-v1-foundation-tests
    COMMAND zevryon-css-declaration-list-v1-tests)

  add_executable(
    zevryon-css-parser-v1-probe
    tests/css_parser_v1_probe.cpp)
  target_link_libraries(
    zevryon-css-parser-v1-probe
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-css-parser-v1-probe)
endif()
