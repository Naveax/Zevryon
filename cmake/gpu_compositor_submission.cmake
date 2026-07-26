if(TARGET zevryon-device-raster-backend)
  add_library(
    zevryon-gpu-compositor-submission STATIC
      src/gpu_compositor_submission.cpp)
  target_include_directories(zevryon-gpu-compositor-submission PUBLIC src)
  target_link_libraries(
    zevryon-gpu-compositor-submission
    PUBLIC zevryon-device-raster-backend)
  zevryon_options(zevryon-gpu-compositor-submission)

  add_executable(
    zevryon-gpu-compositor-submission-benchmark
    src/gpu_compositor_submission_benchmark_main.cpp)
  target_link_libraries(
    zevryon-gpu-compositor-submission-benchmark
    PRIVATE zevryon-gpu-compositor-submission)
  zevryon_options(zevryon-gpu-compositor-submission-benchmark)

  if(MSVC)
    target_compile_options(
      zevryon-gpu-compositor-submission-benchmark PRIVATE /UNDEBUG)
  else()
    target_compile_options(
      zevryon-gpu-compositor-submission-benchmark PRIVATE -UNDEBUG)
  endif()

  if(BUILD_TESTING)
    add_executable(
      zevryon-gpu-compositor-submission-tests
      tests/gpu_compositor_submission_tests.cpp)
    target_link_libraries(
      zevryon-gpu-compositor-submission-tests
      PRIVATE zevryon-gpu-compositor-submission)
    zevryon_options(zevryon-gpu-compositor-submission-tests)

    add_executable(
      zevryon-gpu-compositor-equivalence-tests
      tests/gpu_compositor_equivalence_tests.cpp)
    target_link_libraries(
      zevryon-gpu-compositor-equivalence-tests
      PRIVATE zevryon-gpu-compositor-submission)
    zevryon_options(zevryon-gpu-compositor-equivalence-tests)

    if(MSVC)
      target_compile_options(
        zevryon-gpu-compositor-submission-tests PRIVATE /UNDEBUG)
      target_compile_options(
        zevryon-gpu-compositor-equivalence-tests PRIVATE /UNDEBUG)
    else()
      target_compile_options(
        zevryon-gpu-compositor-submission-tests PRIVATE -UNDEBUG)
      target_compile_options(
        zevryon-gpu-compositor-equivalence-tests PRIVATE -UNDEBUG)
    endif()

    add_test(
      NAME gpu-compositor-submission-tests
      COMMAND zevryon-gpu-compositor-submission-tests)
    add_test(
      NAME gpu-compositor-equivalence-tests
      COMMAND zevryon-gpu-compositor-equivalence-tests)
  endif()
endif()
