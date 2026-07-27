if(TARGET zevryon-native-damage-presentation)
  add_library(
    zevryon-native-platform-adapters STATIC
      src/native_platform_adapters.cpp)
  target_include_directories(zevryon-native-platform-adapters PUBLIC src)
  target_link_libraries(
    zevryon-native-platform-adapters
    PUBLIC zevryon-native-damage-presentation)
  zevryon_options(zevryon-native-platform-adapters)

  add_executable(
    zevryon-native-platform-adapters-benchmark
    src/native_platform_adapters_benchmark_main.cpp)
  target_link_libraries(
    zevryon-native-platform-adapters-benchmark
    PRIVATE zevryon-native-platform-adapters)
  zevryon_options(zevryon-native-platform-adapters-benchmark)

  if(MSVC)
    target_compile_options(
      zevryon-native-platform-adapters-benchmark PRIVATE /UNDEBUG)
  else()
    target_compile_options(
      zevryon-native-platform-adapters-benchmark PRIVATE -UNDEBUG)
  endif()

  if(BUILD_TESTING)
    add_executable(
      zevryon-native-platform-adapters-tests
      tests/native_platform_adapters_tests.cpp)
    target_link_libraries(
      zevryon-native-platform-adapters-tests
      PRIVATE zevryon-native-platform-adapters)
    zevryon_options(zevryon-native-platform-adapters-tests)

    add_executable(
      zevryon-native-platform-adapters-equivalence-tests
      tests/native_platform_adapters_equivalence_tests.cpp)
    target_link_libraries(
      zevryon-native-platform-adapters-equivalence-tests
      PRIVATE zevryon-native-platform-adapters)
    zevryon_options(zevryon-native-platform-adapters-equivalence-tests)

    if(MSVC)
      target_compile_options(
        zevryon-native-platform-adapters-tests PRIVATE /UNDEBUG)
      target_compile_options(
        zevryon-native-platform-adapters-equivalence-tests PRIVATE /UNDEBUG)
    else()
      target_compile_options(
        zevryon-native-platform-adapters-tests PRIVATE -UNDEBUG)
      target_compile_options(
        zevryon-native-platform-adapters-equivalence-tests PRIVATE -UNDEBUG)
    endif()

    add_test(
      NAME native-platform-adapters-tests
      COMMAND zevryon-native-platform-adapters-tests)
    add_test(
      NAME native-platform-adapters-equivalence-tests
      COMMAND zevryon-native-platform-adapters-equivalence-tests)
  endif()
endif()

# Z2F-8A binds concrete native SDK execution behind the certified adapter ABI.
include("${CMAKE_CURRENT_LIST_DIR}/native_gpu_sdk_execution.cmake")

# Z2F-8B1 freezes one-device window swapchain ownership and recreation.
include("${CMAKE_CURRENT_LIST_DIR}/native_window_swapchain.cmake")
