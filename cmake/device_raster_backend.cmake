if(TARGET zevryon-glyph-atlas-submission)
  add_library(
    zevryon-device-raster-backend STATIC
      src/device_raster_backend.cpp)
  target_include_directories(zevryon-device-raster-backend PUBLIC src)
  target_link_libraries(
    zevryon-device-raster-backend
    PUBLIC zevryon-glyph-atlas-submission)
  zevryon_options(zevryon-device-raster-backend)

  add_executable(
    zevryon-device-raster-backend-benchmark
    src/device_raster_backend_benchmark_main.cpp)
  target_link_libraries(
    zevryon-device-raster-backend-benchmark
    PRIVATE zevryon-device-raster-backend)
  zevryon_options(zevryon-device-raster-backend-benchmark)

  # Release and RelWithDebInfo define NDEBUG globally. This certification
  # benchmark uses assert for fixture and topology invariants, so keep those
  # checks active in every configuration.
  if(MSVC)
    target_compile_options(
      zevryon-device-raster-backend-benchmark PRIVATE /UNDEBUG)
  else()
    target_compile_options(
      zevryon-device-raster-backend-benchmark PRIVATE -UNDEBUG)
  endif()

  if(BUILD_TESTING)
    add_executable(
      zevryon-device-raster-backend-tests
      tests/device_raster_backend_tests.cpp)
    target_link_libraries(
      zevryon-device-raster-backend-tests
      PRIVATE zevryon-device-raster-backend)
    zevryon_options(zevryon-device-raster-backend-tests)

    add_executable(
      zevryon-device-raster-backend-equivalence-tests
      tests/device_raster_backend_equivalence_tests.cpp)
    target_link_libraries(
      zevryon-device-raster-backend-equivalence-tests
      PRIVATE zevryon-device-raster-backend)
    zevryon_options(zevryon-device-raster-backend-equivalence-tests)

    # The focused tests use assert as their fail-fast primitive. Keep assertions
    # active under Release and RelWithDebInfo on all supported compilers.
    if(MSVC)
      target_compile_options(
        zevryon-device-raster-backend-tests PRIVATE /UNDEBUG)
      target_compile_options(
        zevryon-device-raster-backend-equivalence-tests PRIVATE /UNDEBUG)
    else()
      target_compile_options(
        zevryon-device-raster-backend-tests
        PRIVATE -UNDEBUG -Wno-unused-function)
      target_compile_options(
        zevryon-device-raster-backend-equivalence-tests PRIVATE -UNDEBUG)
    endif()

    add_test(
      NAME device-raster-backend-tests
      COMMAND zevryon-device-raster-backend-tests)
    add_test(
      NAME device-raster-backend-equivalence-tests
      COMMAND zevryon-device-raster-backend-equivalence-tests)
  endif()
endif()

include("${CMAKE_CURRENT_LIST_DIR}/gpu_compositor_submission.cmake")
