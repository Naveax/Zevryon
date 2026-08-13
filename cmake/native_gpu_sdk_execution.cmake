set(
  ZEVRYON_NATIVE_GPU_SDK_SOURCES
  src/native_gpu_sdk_execution.cpp
  src/native_gpu_sdk_execution_stub.cpp)
set(ZEVRYON_NATIVE_GPU_SDK_HAS_RUNTIME_BACKEND OFF)

# Z2F-8A owns only the stable Z2F-7 submission ABI. When the complete text and
# adapter graph is unavailable, provide the compact submission lifecycle locally
# instead of pulling HarfBuzz and shaping into native device execution.
if(NOT TARGET zevryon-native-platform-adapters)
  list(APPEND
    ZEVRYON_NATIVE_GPU_SDK_SOURCES
    src/native_platform_submission_standalone.cpp)
endif()

find_package(Vulkan QUIET)
if(Vulkan_FOUND)
  list(APPEND ZEVRYON_NATIVE_GPU_SDK_SOURCES src/native_gpu_sdk_execution_vulkan.cpp)
  set(ZEVRYON_NATIVE_GPU_SDK_HAS_RUNTIME_BACKEND ON)
endif()

if(WIN32)
  list(APPEND ZEVRYON_NATIVE_GPU_SDK_SOURCES src/native_gpu_sdk_execution_d3d12.cpp)
  set(ZEVRYON_NATIVE_GPU_SDK_HAS_RUNTIME_BACKEND ON)
endif()

add_library(
  zevryon-native-gpu-sdk-execution STATIC
  ${ZEVRYON_NATIVE_GPU_SDK_SOURCES})
target_include_directories(zevryon-native-gpu-sdk-execution PUBLIC src)
if(TARGET zevryon-native-platform-adapters)
  target_link_libraries(
    zevryon-native-gpu-sdk-execution
    PUBLIC zevryon-native-platform-adapters)
endif()
zevryon_options(zevryon-native-gpu-sdk-execution)

if(Vulkan_FOUND)
  target_compile_definitions(
    zevryon-native-gpu-sdk-execution PRIVATE ZEVRYON_HAS_VULKAN_SDK=1)
  target_link_libraries(zevryon-native-gpu-sdk-execution PRIVATE Vulkan::Vulkan)
endif()

if(WIN32)
  target_compile_definitions(
    zevryon-native-gpu-sdk-execution PRIVATE ZEVRYON_HAS_D3D12_SDK=1)
  target_link_libraries(
    zevryon-native-gpu-sdk-execution PRIVATE d3d12 dxgi dxguid user32)
endif()

add_executable(
  zevryon-native-gpu-sdk-execution-benchmark
  src/native_gpu_sdk_execution_benchmark_main.cpp)
target_link_libraries(
  zevryon-native-gpu-sdk-execution-benchmark
  PRIVATE zevryon-native-gpu-sdk-execution)
zevryon_options(zevryon-native-gpu-sdk-execution-benchmark)

if(MSVC)
  target_compile_options(
    zevryon-native-gpu-sdk-execution-benchmark PRIVATE /UNDEBUG)
else()
  target_compile_options(
    zevryon-native-gpu-sdk-execution-benchmark PRIVATE -UNDEBUG)
endif()

if(BUILD_TESTING)
  add_executable(
    zevryon-native-gpu-sdk-execution-tests
    tests/native_gpu_sdk_execution_tests.cpp)
  target_link_libraries(
    zevryon-native-gpu-sdk-execution-tests
    PRIVATE zevryon-native-gpu-sdk-execution)
  zevryon_options(zevryon-native-gpu-sdk-execution-tests)

  add_executable(
    zevryon-native-gpu-sdk-execution-equivalence-tests
    tests/native_gpu_sdk_execution_equivalence_tests.cpp)
  target_link_libraries(
    zevryon-native-gpu-sdk-execution-equivalence-tests
    PRIVATE zevryon-native-gpu-sdk-execution)
  zevryon_options(zevryon-native-gpu-sdk-execution-equivalence-tests)

  if(MSVC)
    target_compile_options(
      zevryon-native-gpu-sdk-execution-tests PRIVATE /UNDEBUG)
    target_compile_options(
      zevryon-native-gpu-sdk-execution-equivalence-tests PRIVATE /UNDEBUG)
  else()
    target_compile_options(
      zevryon-native-gpu-sdk-execution-tests PRIVATE -UNDEBUG)
    target_compile_options(
      zevryon-native-gpu-sdk-execution-equivalence-tests PRIVATE -UNDEBUG)
  endif()

  add_test(
    NAME native-gpu-sdk-execution-tests
    COMMAND zevryon-native-gpu-sdk-execution-tests)
  add_test(
    NAME native-gpu-sdk-execution-equivalence-tests
    COMMAND zevryon-native-gpu-sdk-execution-equivalence-tests)

  if(ZEVRYON_NATIVE_GPU_SDK_HAS_RUNTIME_BACKEND)
    add_executable(
      zevryon-native-gpu-sdk-platform-smoke-tests
      tests/native_gpu_sdk_platform_smoke_tests.cpp)
    target_link_libraries(
      zevryon-native-gpu-sdk-platform-smoke-tests
      PRIVATE zevryon-native-gpu-sdk-execution)
    zevryon_options(zevryon-native-gpu-sdk-platform-smoke-tests)
    if(MSVC)
      target_compile_options(
        zevryon-native-gpu-sdk-platform-smoke-tests PRIVATE /UNDEBUG)
    else()
      target_compile_options(
        zevryon-native-gpu-sdk-platform-smoke-tests PRIVATE -UNDEBUG)
    endif()
    add_test(
      NAME native-gpu-sdk-platform-smoke-tests
      COMMAND zevryon-native-gpu-sdk-platform-smoke-tests)
  endif()
endif()
