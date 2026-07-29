if(TARGET zevryon-native-gpu-sdk-execution)
  # Pixel-view validation is independent from the optional compositor and
  # platform presenter libraries. D3D12, Vulkan and Metal minimal graphs
  # therefore remain link-complete without introducing dependency cycles.
  add_library(
    zevryon-native-window-pixel-buffer STATIC
    src/native_window_pixel_buffer.cpp)
  target_include_directories(zevryon-native-window-pixel-buffer PUBLIC src)
  target_link_libraries(
    zevryon-native-window-pixel-buffer
    PUBLIC zevryon-native-gpu-sdk-execution)
  zevryon_options(zevryon-native-window-pixel-buffer)

  set(
    ZEVRYON_NATIVE_WINDOW_SWAPCHAIN_SOURCES
    src/native_window_swapchain.cpp
    src/native_window_swapchain_stub.cpp)

  if(WIN32)
    list(APPEND
      ZEVRYON_NATIVE_WINDOW_SWAPCHAIN_SOURCES
      src/native_window_swapchain_d3d12.cpp)
  endif()

  add_library(
    zevryon-native-window-swapchain STATIC
    ${ZEVRYON_NATIVE_WINDOW_SWAPCHAIN_SOURCES})
  target_include_directories(zevryon-native-window-swapchain PUBLIC src)
  target_link_libraries(
    zevryon-native-window-swapchain
    PUBLIC
      zevryon-native-gpu-sdk-execution
      zevryon-native-window-pixel-buffer)
  if(WIN32)
    target_compile_definitions(
      zevryon-native-window-swapchain
      PRIVATE ZEVRYON_HAS_D3D12_WINDOW_SWAPCHAIN=1)
    target_link_libraries(
      zevryon-native-window-swapchain PRIVATE d3d12 dxgi dxguid user32)
  endif()
  zevryon_options(zevryon-native-window-swapchain)

  add_executable(
    zevryon-native-window-swapchain-benchmark
    src/native_window_swapchain_benchmark_main.cpp)
  target_link_libraries(
    zevryon-native-window-swapchain-benchmark
    PRIVATE zevryon-native-window-swapchain)
  zevryon_options(zevryon-native-window-swapchain-benchmark)

  if(WIN32)
    add_executable(
      zevryon-native-window-swapchain-d3d12-benchmark
      src/native_window_swapchain_d3d12_benchmark_main.cpp)
    target_link_libraries(
      zevryon-native-window-swapchain-d3d12-benchmark
      PRIVATE zevryon-native-window-swapchain)
    zevryon_options(zevryon-native-window-swapchain-d3d12-benchmark)
  endif()

  if(MSVC)
    target_compile_options(
      zevryon-native-window-swapchain-benchmark PRIVATE /UNDEBUG)
    if(TARGET zevryon-native-window-swapchain-d3d12-benchmark)
      target_compile_options(
        zevryon-native-window-swapchain-d3d12-benchmark PRIVATE /UNDEBUG)
    endif()
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

    if(WIN32)
      add_executable(
        zevryon-native-window-swapchain-d3d12-tests
        tests/native_window_swapchain_d3d12_tests.cpp)
      target_link_libraries(
        zevryon-native-window-swapchain-d3d12-tests
        PRIVATE zevryon-native-window-swapchain)
      zevryon_options(zevryon-native-window-swapchain-d3d12-tests)
    endif()

    if(MSVC)
      target_compile_options(
        zevryon-native-window-swapchain-tests PRIVATE /UNDEBUG)
      target_compile_options(
        zevryon-native-window-swapchain-equivalence-tests PRIVATE /UNDEBUG)
      if(TARGET zevryon-native-window-swapchain-d3d12-tests)
        target_compile_options(
          zevryon-native-window-swapchain-d3d12-tests PRIVATE /UNDEBUG)
      endif()
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
    if(TARGET zevryon-native-window-swapchain-d3d12-tests)
      add_test(
        NAME native-window-swapchain-d3d12-tests
        COMMAND zevryon-native-window-swapchain-d3d12-tests)
    endif()
  endif()
endif()

# Z2F-8B2B binds real Vulkan WSI to the retained single-device context.
include("${CMAKE_CURRENT_LIST_DIR}/native_vulkan_wsi.cmake")

# Z2F-8B2C binds real CAMetalLayer presentation to one retained Metal graph.
include("${CMAKE_CURRENT_LIST_DIR}/native_metal_window.cmake")

# Z2F-8B3A composes deterministic pixels and transfers them to native back buffers.
include("${CMAKE_CURRENT_LIST_DIR}/shared_pixel_compositor.cmake")
