target_sources(
  zevryon-massivedoc-core
  PRIVATE
    src/foreground_layout_handoff.cpp)

if(BUILD_TESTING)
  add_executable(
    zevryon-foreground-layout-handoff-tests
    tests/foreground_layout_handoff_tests.cpp)
  target_link_libraries(
    zevryon-foreground-layout-handoff-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-foreground-layout-handoff-tests)
  add_test(
    NAME foreground-layout-handoff-tests
    COMMAND zevryon-foreground-layout-handoff-tests)

  add_executable(
    zevryon-foreground-layout-activity-race-tests
    tests/foreground_layout_activity_race_tests.cpp)
  target_link_libraries(
    zevryon-foreground-layout-activity-race-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-foreground-layout-activity-race-tests)
  add_test(
    NAME foreground-layout-activity-race-tests
    COMMAND zevryon-foreground-layout-activity-race-tests)
endif()
