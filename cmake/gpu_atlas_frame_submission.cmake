if(TARGET zevryon-device-raster-backend)
  add_library(
    zevryon-gpu-atlas-frame-submission STATIC
      src/gpu_atlas_frame_submission.cpp)
  target_include_directories(zevryon-gpu-atlas-frame-submission PUBLIC src)
  target_link_libraries(
    zevryon-gpu-atlas-frame-submission
    PUBLIC zevryon-device-raster-backend)
  zevryon_options(zevryon-gpu-atlas-frame-submission)

  add_executable(
    zevryon-gpu-atlas-frame-submission-benchmark
    src/gpu_atlas_frame_submission_benchmark_main.cpp)
  target_link_libraries(
    zevryon-gpu-atlas-frame-submission-benchmark
    PRIVATE zevryon-gpu-atlas-frame-submission)
  zevryon_options(zevryon-gpu-atlas-frame-submission-benchmark)

  if(MSVC)
    target_compile_options(
      zevryon-gpu-atlas-frame-submission-benchmark PRIVATE /UNDEBUG)
  else()
    target_compile_options(
      zevryon-gpu-atlas-frame-submission-benchmark PRIVATE -UNDEBUG)
  endif()

  if(BUILD_TESTING)
    add_executable(
      zevryon-gpu-atlas-frame-submission-tests
      tests/gpu_atlas_frame_submission_tests.cpp)
    target_link_libraries(
      zevryon-gpu-atlas-frame-submission-tests
      PRIVATE zevryon-gpu-atlas-frame-submission)
    zevryon_options(zevryon-gpu-atlas-frame-submission-tests)

    add_executable(
      zevryon-gpu-atlas-frame-submission-equivalence-tests
      tests/gpu_atlas_frame_submission_equivalence_tests.cpp)
    target_link_libraries(
      zevryon-gpu-atlas-frame-submission-equivalence-tests
      PRIVATE zevryon-gpu-atlas-frame-submission)
    zevryon_options(zevryon-gpu-atlas-frame-submission-equivalence-tests)

    if(MSVC)
      target_compile_options(
        zevryon-gpu-atlas-frame-submission-tests PRIVATE /UNDEBUG)
      target_compile_options(
        zevryon-gpu-atlas-frame-submission-equivalence-tests PRIVATE /UNDEBUG)
    else()
      target_compile_options(
        zevryon-gpu-atlas-frame-submission-tests PRIVATE -UNDEBUG)
      target_compile_options(
        zevryon-gpu-atlas-frame-submission-equivalence-tests PRIVATE -UNDEBUG)
    endif()

    add_test(
      NAME gpu-atlas-frame-submission-tests
      COMMAND zevryon-gpu-atlas-frame-submission-tests)
    add_test(
      NAME gpu-atlas-frame-submission-equivalence-tests
      COMMAND zevryon-gpu-atlas-frame-submission-equivalence-tests)
  endif()
endif()
