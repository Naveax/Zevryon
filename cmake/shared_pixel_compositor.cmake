if(TARGET zevryon-gpu-atlas-frame-submission AND
   TARGET zevryon-native-window-swapchain AND
   TARGET zevryon-native-window-pixel-buffer)
  add_library(
    zevryon-shared-pixel-compositor STATIC
    src/shared_pixel_compositor.cpp)
  target_include_directories(zevryon-shared-pixel-compositor PUBLIC src)
  target_link_libraries(
    zevryon-shared-pixel-compositor
    PUBLIC
      zevryon-gpu-atlas-frame-submission
      zevryon-native-window-pixel-buffer)
  zevryon_options(zevryon-shared-pixel-compositor)

  target_link_libraries(
    zevryon-native-window-swapchain
    PUBLIC zevryon-shared-pixel-compositor)

  add_executable(
    zevryon-shared-pixel-compositor-benchmark
    src/shared_pixel_compositor_benchmark_main.cpp)
  target_link_libraries(
    zevryon-shared-pixel-compositor-benchmark
    PRIVATE zevryon-shared-pixel-compositor)
  zevryon_options(zevryon-shared-pixel-compositor-benchmark)

  if(MSVC)
    target_compile_options(
      zevryon-shared-pixel-compositor-benchmark PRIVATE /UNDEBUG)
  else()
    target_compile_options(
      zevryon-shared-pixel-compositor-benchmark PRIVATE -UNDEBUG)
  endif()

  if(BUILD_TESTING)
    add_executable(
      zevryon-shared-pixel-compositor-tests
      tests/shared_pixel_compositor_tests.cpp)
    target_link_libraries(
      zevryon-shared-pixel-compositor-tests
      PRIVATE zevryon-shared-pixel-compositor)
    zevryon_options(zevryon-shared-pixel-compositor-tests)

    add_executable(
      zevryon-shared-pixel-compositor-equivalence-tests
      tests/shared_pixel_compositor_equivalence_tests.cpp)
    target_link_libraries(
      zevryon-shared-pixel-compositor-equivalence-tests
      PRIVATE zevryon-shared-pixel-compositor)
    zevryon_options(zevryon-shared-pixel-compositor-equivalence-tests)

    if(MSVC)
      target_compile_options(
        zevryon-shared-pixel-compositor-tests PRIVATE /UNDEBUG)
      target_compile_options(
        zevryon-shared-pixel-compositor-equivalence-tests PRIVATE /UNDEBUG)
    else()
      target_compile_options(
        zevryon-shared-pixel-compositor-tests PRIVATE -UNDEBUG)
      target_compile_options(
        zevryon-shared-pixel-compositor-equivalence-tests PRIVATE -UNDEBUG)
    endif()

    add_test(
      NAME shared-pixel-compositor-tests
      COMMAND zevryon-shared-pixel-compositor-tests)
    add_test(
      NAME shared-pixel-compositor-equivalence-tests
      COMMAND zevryon-shared-pixel-compositor-equivalence-tests)
  endif()
endif()
