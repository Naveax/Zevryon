target_sources(
  zevryon-massivedoc-core
  PRIVATE
    src/frame_budget_scheduler.cpp
    src/hot_scroll_source_prefetch.cpp
    src/prefetch_tail_admission.cpp
    src/shared_source_prefetch_pool.cpp
    src/velocity_prefetch_planner.cpp
    src/zenith_process_tab_controller.cpp
    src/zenith_tab_runtime.cpp)

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

  add_executable(
    zevryon-shared-source-prefetch-pool-tests
    tests/shared_source_prefetch_pool_tests.cpp)
  target_link_libraries(
    zevryon-shared-source-prefetch-pool-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-shared-source-prefetch-pool-tests)
  add_test(
    NAME shared-source-prefetch-pool-tests
    COMMAND zevryon-shared-source-prefetch-pool-tests)

  add_executable(
    zevryon-velocity-prefetch-planner-tests
    tests/velocity_prefetch_planner_tests.cpp)
  target_link_libraries(
    zevryon-velocity-prefetch-planner-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-velocity-prefetch-planner-tests)
  add_test(
    NAME velocity-prefetch-planner-tests
    COMMAND zevryon-velocity-prefetch-planner-tests)

  add_executable(
    zevryon-prefetch-tail-admission-tests
    tests/prefetch_tail_admission_tests.cpp)
  target_link_libraries(
    zevryon-prefetch-tail-admission-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-prefetch-tail-admission-tests)
  add_test(
    NAME prefetch-tail-admission-tests
    COMMAND zevryon-prefetch-tail-admission-tests)

  add_executable(
    zevryon-unbounded-tab-registry-tests
    tests/unbounded_tab_registry_tests.cpp)
  target_link_libraries(
    zevryon-unbounded-tab-registry-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-unbounded-tab-registry-tests)
  add_test(
    NAME unbounded-tab-registry-tests
    COMMAND zevryon-unbounded-tab-registry-tests)

  add_executable(
    zevryon-process-tab-pressure-controller-tests
    tests/zenith_process_tab_controller_tests.cpp)
  target_link_libraries(
    zevryon-process-tab-pressure-controller-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-process-tab-pressure-controller-tests)
  add_test(
    NAME process-tab-pressure-controller-tests
    COMMAND zevryon-process-tab-pressure-controller-tests)

  add_executable(
    zevryon-tab-runtime-tests
    tests/zenith_tab_runtime_tests.cpp)
  target_link_libraries(
    zevryon-tab-runtime-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-tab-runtime-tests)
  add_test(
    NAME zenith-tab-runtime-tests
    COMMAND zevryon-tab-runtime-tests)
endif()
