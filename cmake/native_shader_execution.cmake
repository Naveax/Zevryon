set(
  ZEVRYON_NATIVE_SHADER_EXECUTION_SOURCES
  src/native_shader_execution_stub.cpp)

if(WIN32)
  list(APPEND
    ZEVRYON_NATIVE_SHADER_EXECUTION_SOURCES
    src/native_shader_execution_d3d12.cpp)
endif()

add_library(
  zevryon-native-shader-execution STATIC
  ${ZEVRYON_NATIVE_SHADER_EXECUTION_SOURCES})
target_include_directories(zevryon-native-shader-execution PUBLIC src)
target_link_libraries(
  zevryon-native-shader-execution
  PUBLIC zevryon-shader-draw-packet)
zevryon_options(zevryon-native-shader-execution)

if(WIN32)
  target_compile_definitions(
    zevryon-native-shader-execution
    PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN ZEVRYON_HAS_D3D12_SHADER_EXECUTION=1)
  target_link_libraries(
    zevryon-native-shader-execution
    PRIVATE d3d12 dxgi dxguid d3dcompiler)
endif()

if(BUILD_TESTING)
  add_executable(
    zevryon-native-shader-execution-stub-tests
    tests/native_shader_execution_stub_tests.cpp)
  target_link_libraries(
    zevryon-native-shader-execution-stub-tests
    PRIVATE zevryon-native-shader-execution)
  zevryon_options(zevryon-native-shader-execution-stub-tests)
  add_test(
    NAME native-shader-execution-stub-tests
    COMMAND zevryon-native-shader-execution-stub-tests)

  if(WIN32)
    add_executable(
      zevryon-native-shader-execution-d3d12-tests
      tests/native_shader_execution_d3d12_tests.cpp)
    target_include_directories(
      zevryon-native-shader-execution-d3d12-tests PRIVATE tests)
    target_link_libraries(
      zevryon-native-shader-execution-d3d12-tests
      PRIVATE zevryon-native-shader-execution zevryon-native-gpu-sdk-execution)
    zevryon_options(zevryon-native-shader-execution-d3d12-tests)

    add_executable(
      zevryon-native-shader-execution-d3d12-benchmark
      src/native_shader_execution_d3d12_benchmark_main.cpp)
    target_include_directories(
      zevryon-native-shader-execution-d3d12-benchmark PRIVATE tests)
    target_link_libraries(
      zevryon-native-shader-execution-d3d12-benchmark
      PRIVATE zevryon-native-shader-execution zevryon-native-gpu-sdk-execution)
    zevryon_options(zevryon-native-shader-execution-d3d12-benchmark)

    target_compile_options(
      zevryon-native-shader-execution-d3d12-tests PRIVATE /UNDEBUG)
    target_compile_options(
      zevryon-native-shader-execution-d3d12-benchmark PRIVATE /UNDEBUG)

    add_test(
      NAME native-shader-execution-d3d12-tests
      COMMAND zevryon-native-shader-execution-d3d12-tests)
  endif()
endif()

# Z2F-8B3B2B binds the shared packet to retained Vulkan compute execution.
include("${CMAKE_CURRENT_LIST_DIR}/native_vulkan_shader_execution.cmake")
# Z2F-8B3B2C binds the shared packet to retained Metal compute execution.
include("${CMAKE_CURRENT_LIST_DIR}/native_metal_shader_execution.cmake")
