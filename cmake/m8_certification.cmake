if(BUILD_TESTING)
  add_executable(
    zevryon-m8-storage-crash-cut-tests
    tests/m8_storage_crash_cut_tests.cpp)
  target_link_libraries(
    zevryon-m8-storage-crash-cut-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-m8-storage-crash-cut-tests)
  add_test(
    NAME m8-storage-crash-cut-tests
    COMMAND zevryon-m8-storage-crash-cut-tests)

  add_executable(
    zevryon-m8-storage-crash-probe
    tests/m8_storage_crash_probe.cpp)
  target_link_libraries(
    zevryon-m8-storage-crash-probe
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-m8-storage-crash-probe)

  add_executable(
    zevryon-m8-mixed-mutation-probe
    tests/m8_mixed_mutation_probe.cpp)
  target_link_libraries(
    zevryon-m8-mixed-mutation-probe
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-m8-mixed-mutation-probe)

  add_executable(
    zevryon-m8-continuous-soak-probe
    tests/m8_continuous_soak_probe.cpp)
  target_link_libraries(
    zevryon-m8-continuous-soak-probe
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-m8-continuous-soak-probe)

  add_executable(
    zevryon-m8-property-fuzz-probe
    tests/m8_property_fuzz_probe.cpp)
  target_link_libraries(
    zevryon-m8-property-fuzz-probe
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-m8-property-fuzz-probe)

  add_executable(
    zevryon-m8-profile-runtime-policy-probe
    tests/m8_profile_runtime_policy_probe.cpp)
  target_link_libraries(
    zevryon-m8-profile-runtime-policy-probe
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-m8-profile-runtime-policy-probe)

  add_executable(
    zevryon-m8-profile-case-probe
    tests/m8_profile_case_probe.cpp)
  target_link_libraries(
    zevryon-m8-profile-case-probe
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-m8-profile-case-probe)

  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(
      NAME m8-storage-process-crash-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/m8_storage_process_crash_tests.py"
        --probe "$<TARGET_FILE:zevryon-m8-storage-crash-probe>"
        --work-dir "${CMAKE_CURRENT_BINARY_DIR}/m8-storage-process-crash"
        --output "${CMAKE_CURRENT_BINARY_DIR}/evidence/m8/storage-process-crash.json")

    add_test(
      NAME m8-profile-observation-gate-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/m8_profile_observation_gate_tests.py")

    add_test(
      NAME m8-titan-fixture-authority-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/m8_titan_fixture_tests.py"
        --work-dir "${CMAKE_CURRENT_BINARY_DIR}/m8-titan-fixture-smoke")

    add_test(
      NAME m8-titan-fixture-utf8-authority-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/m8_titan_fixture_utf8_tests.py"
        --work-dir "${CMAKE_CURRENT_BINARY_DIR}/m8-titan-fixture-utf8-smoke")

    add_test(
      NAME m8-profile-runtime-policy-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/m8_profile_runtime_policy_tests.py"
        --probe "$<TARGET_FILE:zevryon-m8-profile-runtime-policy-probe>")

    add_test(
      NAME m8-profile-collection-binder-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/m8_profile_collection_binder_tests.py")

    add_test(
      NAME m8-profile-collection-binder-v2-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/m8_profile_collection_binder_v2_tests.py")

    add_test(
      NAME m8-profile-bundle-importer-v2-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/m8_bundle_import_profile_tests.py")

    add_test(
      NAME m8-final-profile-receipt-v2-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/m8_final_evidence_profile_receipt_v2_tests.py")

    add_test(
      NAME m8-profile-case-probe-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/m8_profile_case_probe_utf8_tests.py"
        --probe "$<TARGET_FILE:zevryon-m8-profile-case-probe>"
        --work-dir "${CMAKE_CURRENT_BINARY_DIR}/m8-profile-case-probe-smoke")

    add_test(
      NAME m8-profile-case-collector-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/m8_profile_case_collector_tests.py")

    add_test(
      NAME m8-profile-case-provenance-verifier-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/m8_profile_case_provenance_verifier_tests.py")

    add_test(
      NAME m8-mixed-mutation-authority-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/m8_mixed_mutation_probe_tests.py"
        --probe "$<TARGET_FILE:zevryon-m8-mixed-mutation-probe>"
        --work-dir "${CMAKE_CURRENT_BINARY_DIR}/m8-mixed-mutation-smoke")

    add_test(
      NAME m8-continuous-soak-authority-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/m8_continuous_soak_probe_tests.py"
        --probe "$<TARGET_FILE:zevryon-m8-continuous-soak-probe>"
        --work-dir "${CMAKE_CURRENT_BINARY_DIR}/m8-continuous-soak-smoke")

    add_test(
      NAME m8-property-fuzz-authority-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/m8_property_fuzz_probe_tests.py"
        --probe "$<TARGET_FILE:zevryon-m8-property-fuzz-probe>"
        --work-dir "${CMAKE_CURRENT_BINARY_DIR}/m8-property-fuzz-smoke")
  endif()
endif()
