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
endif()
