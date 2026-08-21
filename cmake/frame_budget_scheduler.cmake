target_sources(
  zevryon-massivedoc-core
  PRIVATE
    src/device_frame_profile.cpp
    src/frame_budget_scheduler.cpp
    src/hot_scroll_source_prefetch.cpp
    src/prefetch_record_bounds.cpp
    src/prefetch_tail_admission.cpp
    src/runtime_prefetch_record_policy.cpp
    src/shared_record_length_authority.cpp
    src/shared_source_prefetch_pool.cpp
    src/velocity_prefetch_planner.cpp
    src/zenith_process_tab_controller.cpp
    src/zenith_tab_runtime.cpp
    src/zenith_tab_runtime_profile.cpp)

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
    zevryon-shared-record-length-authority-tests
    tests/shared_record_length_authority_tests.cpp)
  target_link_libraries(
    zevryon-shared-record-length-authority-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-shared-record-length-authority-tests)
  add_test(
    NAME shared-record-length-authority-tests
    COMMAND zevryon-shared-record-length-authority-tests)

  add_executable(
    zevryon-runtime-record-length-policy-tests
    tests/runtime_prefetch_record_policy_tests.cpp)
  target_link_libraries(
    zevryon-runtime-record-length-policy-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-runtime-record-length-policy-tests)
  add_test(
    NAME runtime-record-length-policy-tests
    COMMAND zevryon-runtime-record-length-policy-tests)

  add_executable(
    zevryon-device-frame-profile-tests
    tests/device_frame_profile_tests.cpp)
  target_link_libraries(
    zevryon-device-frame-profile-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-device-frame-profile-tests)
  add_test(
    NAME device-frame-profile-tests
    COMMAND zevryon-device-frame-profile-tests)

  add_executable(
    zevryon-frame-profile-scheduler-certification-tests
    tests/frame_profile_scheduler_certification_tests.cpp)
  target_link_libraries(
    zevryon-frame-profile-scheduler-certification-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-frame-profile-scheduler-certification-tests)
  add_test(
    NAME frame-profile-scheduler-certification-tests
    COMMAND zevryon-frame-profile-scheduler-certification-tests)

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
