if(APPLE AND TARGET zevryon-native-shader-execution AND
   TARGET zevryon-native-gpu-sdk-execution)
  target_sources(
    zevryon-native-shader-execution
    PRIVATE src/native_shader_execution_metal.mm)
  set_source_files_properties(
    src/native_shader_execution_metal.mm
    PROPERTIES COMPILE_FLAGS "-fobjc-arc")
  target_compile_definitions(
    zevryon-native-shader-execution
    PRIVATE ZEVRYON_HAS_METAL_SHADER_EXECUTION=1)
  target_link_libraries(
    zevryon-native-shader-execution
    PRIVATE
      "-framework Foundation"
      "-framework Metal")

  if(BUILD_TESTING AND TARGET zevryon-metal-window-test-host)
    add_executable(
      zevryon-native-shader-execution-metal-tests
      tests/native_shader_execution_metal_tests.cpp)
    target_include_directories(
      zevryon-native-shader-execution-metal-tests PRIVATE tests)
    target_link_libraries(
      zevryon-native-shader-execution-metal-tests
      PRIVATE
        zevryon-native-shader-execution
        zevryon-native-gpu-sdk-execution
        zevryon-metal-window-test-host)
    zevryon_options(zevryon-native-shader-execution-metal-tests)
    target_compile_options(
      zevryon-native-shader-execution-metal-tests PRIVATE -UNDEBUG)
    add_test(
      NAME native-shader-execution-metal-tests
      COMMAND zevryon-native-shader-execution-metal-tests)

    add_executable(
      zevryon-native-shader-execution-metal-benchmark
      src/native_shader_execution_metal_benchmark_main.cpp)
    target_include_directories(
      zevryon-native-shader-execution-metal-benchmark PRIVATE tests)
    target_link_libraries(
      zevryon-native-shader-execution-metal-benchmark
      PRIVATE
        zevryon-native-shader-execution
        zevryon-native-gpu-sdk-execution
        zevryon-metal-window-test-host)
    zevryon_options(zevryon-native-shader-execution-metal-benchmark)
    target_compile_options(
      zevryon-native-shader-execution-metal-benchmark PRIVATE -UNDEBUG)
  endif()
endif()
