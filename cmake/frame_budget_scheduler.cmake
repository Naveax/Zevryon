target_sources(
  zevryon-massivedoc-core
  PRIVATE
    src/frame_budget_scheduler.cpp
    src/hot_scroll_source_prefetch.cpp)

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

  add_executable(
    zevryon-hot-scroll-source-prefetch-tests
    tests/hot_scroll_source_prefetch_tests.cpp)
  target_link_libraries(
    zevryon-hot-scroll-source-prefetch-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-hot-scroll-source-prefetch-tests)
  add_test(
    NAME hot-scroll-source-prefetch-tests
    COMMAND zevryon-hot-scroll-source-prefetch-tests)
endif()
