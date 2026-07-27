if(TARGET zevryon-gpu-atlas-frame-submission)
  add_library(
    zevryon-gpu-device-presentation STATIC
      src/gpu_device_presentation.cpp)
  target_include_directories(zevryon-gpu-device-presentation PUBLIC src)
  target_link_libraries(
    zevryon-gpu-device-presentation
    PUBLIC zevryon-gpu-atlas-frame-submission)
  zevryon_options(zevryon-gpu-device-presentation)

  add_executable(
    zevryon-gpu-device-presentation-benchmark
    src/gpu_device_presentation_benchmark_main.cpp)
  target_link_libraries(
    zevryon-gpu-device-presentation-benchmark
    PRIVATE zevryon-gpu-device-presentation)
  zevryon_options(zevryon-gpu-device-presentation-benchmark)

  if(MSVC)
    target_compile_options(
      zevryon-gpu-device-presentation-benchmark PRIVATE /UNDEBUG)
  else()
    target_compile_options(
      zevryon-gpu-device-presentation-benchmark PRIVATE -UNDEBUG)
  endif()

  if(BUILD_TESTING)
    add_executable(
      zevryon-gpu-device-presentation-tests
      tests/gpu_device_presentation_tests.cpp)
    target_link_libraries(
      zevryon-gpu-device-presentation-tests
      PRIVATE zevryon-gpu-device-presentation)
    zevryon_options(zevryon-gpu-device-presentation-tests)

    add_executable(
      zevryon-gpu-device-presentation-equivalence-tests
      tests/gpu_device_presentation_equivalence_tests.cpp)
    target_link_libraries(
      zevryon-gpu-device-presentation-equivalence-tests
      PRIVATE zevryon-gpu-device-presentation)
    zevryon_options(zevryon-gpu-device-presentation-equivalence-tests)

    if(MSVC)
      target_compile_options(
        zevryon-gpu-device-presentation-tests PRIVATE /UNDEBUG)
      target_compile_options(
        zevryon-gpu-device-presentation-equivalence-tests PRIVATE /UNDEBUG)
    else()
      target_compile_options(
        zevryon-gpu-device-presentation-tests PRIVATE -UNDEBUG)
      target_compile_options(
        zevryon-gpu-device-presentation-equivalence-tests PRIVATE -UNDEBUG)
    endif()

    add_test(
      NAME gpu-device-presentation-tests
      COMMAND zevryon-gpu-device-presentation-tests)
    add_test(
      NAME gpu-device-presentation-equivalence-tests
      COMMAND zevryon-gpu-device-presentation-equivalence-tests)
  endif()
endif()

include("${CMAKE_CURRENT_LIST_DIR}/native_damage_presentation.cmake")
