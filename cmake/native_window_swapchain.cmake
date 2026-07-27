if(TARGET zevryon-native-gpu-sdk-execution)
  add_library(
    zevryon-native-window-swapchain STATIC
    src/native_window_swapchain.cpp)
  target_include_directories(zevryon-native-window-swapchain PUBLIC src)
  target_link_libraries(
    zevryon-native-window-swapchain
    PUBLIC zevryon-native-gpu-sdk-execution)
  zevryon_options(zevryon-native-window-swapchain)

  add_executable(
    zevryon-native-window-swapchain-benchmark
    src/native_window_swapchain_benchmark_main.cpp)
  target_link_libraries(
    zevryon-native-window-swapchain-benchmark
    PRIVATE zevryon-native-window-swapchain)
  zevryon_options(zevryon-native-window-swapchain-benchmark)

  if(MSVC)
    target_compile_options(
      zevryon-native-window-swapchain-benchmark PRIVATE /UNDEBUG)
  else()
    target_compile_options(
      zevryon-native-window-swapchain-benchmark PRIVATE -UNDEBUG)
  endif()

  if(BUILD_TESTING)
    add_executable(
      zevryon-native-window-swapchain-tests
      tests/native_window_swapchain_tests.cpp)
    target_link_libraries(
      zevryon-native-window-swapchain-tests
      PRIVATE zevryon-native-window-swapchain)
    zevryon_options(zevryon-native-window-swapchain-tests)

    add_executable(
      zevryon-native-window-swapchain-equivalence-tests
      tests/native_window_swapchain_equivalence_tests.cpp)
    target_link_libraries(
      zevryon-native-window-swapchain-equivalence-tests
      PRIVATE zevryon-native-window-swapchain)
    zevryon_options(zevryon-native-window-swapchain-equivalence-tests)

    if(MSVC)
      target_compile_options(
        zevryon-native-window-swapchain-tests PRIVATE /UNDEBUG)
      target_compile_options(
        zevryon-native-window-swapchain-equivalence-tests PRIVATE /UNDEBUG)
    else()
      target_compile_options(
        zevryon-native-window-swapchain-tests PRIVATE -UNDEBUG)
      target_compile_options(
        zevryon-native-window-swapchain-equivalence-tests PRIVATE -UNDEBUG)
    endif()

    add_test(
      NAME native-window-swapchain-tests
      COMMAND zevryon-native-window-swapchain-tests)
    add_test(
      NAME native-window-swapchain-equivalence-tests
      COMMAND zevryon-native-window-swapchain-equivalence-tests)
  endif()
endif()
