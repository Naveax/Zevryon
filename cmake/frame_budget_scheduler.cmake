target_sources(
  zevryon-massivedoc-core
  PRIVATE
    src/frame_budget_scheduler.cpp)

if(BUILD_TESTING)
  add_executable(
    zevryon-frame-budget-scheduler-tests
    tests/frame_budget_scheduler_tests.cpp)
  target_link_libraries(
    zevryon-frame-budget-scheduler-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-frame-budget-scheduler-tests)
  add_test(
    NAME frame-budget-scheduler-tests
    COMMAND zevryon-frame-budget-scheduler-tests)
endif()
