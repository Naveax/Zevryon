if(TARGET zevryon-native-gpu-sdk-execution AND
   TARGET zevryon-native-window-swapchain)
  target_sources(
    zevryon-native-gpu-sdk-execution
    PRIVATE src/native_metal_window_stub.cpp)

  if(APPLE)
    target_sources(
      zevryon-native-gpu-sdk-execution
      PRIVATE src/native_gpu_sdk_execution_metal_window.mm)
    target_sources(
      zevryon-native-window-swapchain
      PRIVATE src/native_window_swapchain_metal.mm)

    set_source_files_properties(
      src/native_gpu_sdk_execution_metal_window.mm
      src/native_window_swapchain_metal.mm
      PROPERTIES COMPILE_FLAGS "-fobjc-arc")

    target_compile_definitions(
      zevryon-native-gpu-sdk-execution
      PRIVATE ZEVRYON_HAS_METAL_WINDOW_SWAPCHAIN=1)
    target_compile_definitions(
      zevryon-native-window-swapchain
      PRIVATE ZEVRYON_HAS_METAL_WINDOW_SWAPCHAIN=1)

    target_link_libraries(
      zevryon-native-gpu-sdk-execution
      PRIVATE
        "-framework AppKit"
        "-framework Foundation"
        "-framework Metal"
        "-framework QuartzCore")
    target_link_libraries(
      zevryon-native-window-swapchain
      PRIVATE
        "-framework AppKit"
        "-framework Foundation"
        "-framework Metal"
        "-framework QuartzCore")

    if(BUILD_TESTING)
      add_library(
        zevryon-metal-window-test-host STATIC
        tests/native_metal_window_test_window.mm)
      set_source_files_properties(
        tests/native_metal_window_test_window.mm
        PROPERTIES COMPILE_FLAGS "-fobjc-arc")
      target_include_directories(
        zevryon-metal-window-test-host
        PUBLIC tests src)
      target_link_libraries(
        zevryon-metal-window-test-host
        PUBLIC zevryon-native-window-swapchain
        PRIVATE
          "-framework AppKit"
          "-framework Foundation"
          "-framework QuartzCore")
      zevryon_options(zevryon-metal-window-test-host)

      add_executable(
        zevryon-native-window-swapchain-metal-tests
        tests/native_window_swapchain_metal_tests.cpp)
      target_link_libraries(
        zevryon-native-window-swapchain-metal-tests
        PRIVATE zevryon-metal-window-test-host)
      zevryon_options(zevryon-native-window-swapchain-metal-tests)

      add_executable(
        zevryon-native-window-swapchain-metal-benchmark
        src/native_window_swapchain_metal_benchmark_main.cpp)
      target_link_libraries(
        zevryon-native-window-swapchain-metal-benchmark
        PRIVATE zevryon-metal-window-test-host)
      zevryon_options(zevryon-native-window-swapchain-metal-benchmark)

      target_compile_options(
        zevryon-native-window-swapchain-metal-tests PRIVATE -UNDEBUG)
      target_compile_options(
        zevryon-native-window-swapchain-metal-benchmark PRIVATE -UNDEBUG)

      add_test(
        NAME native-window-swapchain-metal-tests
        COMMAND zevryon-native-window-swapchain-metal-tests)
    endif()
  endif()
endif()
