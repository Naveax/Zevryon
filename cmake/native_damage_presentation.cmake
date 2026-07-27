if(TARGET zevryon-gpu-device-presentation)
  add_library(
    zevryon-native-damage-presentation STATIC
      src/native_damage_presentation.cpp)
  target_include_directories(zevryon-native-damage-presentation PUBLIC src)
  target_link_libraries(
    zevryon-native-damage-presentation
    PUBLIC zevryon-gpu-device-presentation)
  zevryon_options(zevryon-native-damage-presentation)

  add_executable(
    zevryon-native-damage-presentation-benchmark
    src/native_damage_presentation_benchmark_main.cpp)
  target_link_libraries(
    zevryon-native-damage-presentation-benchmark
    PRIVATE zevryon-native-damage-presentation)
  zevryon_options(zevryon-native-damage-presentation-benchmark)

  if(MSVC)
    target_compile_options(
      zevryon-native-damage-presentation-benchmark PRIVATE /UNDEBUG)
  else()
    target_compile_options(
      zevryon-native-damage-presentation-benchmark PRIVATE -UNDEBUG)
  endif()

  if(BUILD_TESTING)
    add_executable(
      zevryon-native-damage-presentation-tests
      tests/native_damage_presentation_tests.cpp)
    target_link_libraries(
      zevryon-native-damage-presentation-tests
      PRIVATE zevryon-native-damage-presentation)
    zevryon_options(zevryon-native-damage-presentation-tests)

    add_executable(
      zevryon-native-damage-presentation-equivalence-tests
      tests/native_damage_presentation_equivalence_tests.cpp)
    target_link_libraries(
      zevryon-native-damage-presentation-equivalence-tests
      PRIVATE zevryon-native-damage-presentation)
    zevryon_options(zevryon-native-damage-presentation-equivalence-tests)

    if(MSVC)
      target_compile_options(
        zevryon-native-damage-presentation-tests PRIVATE /UNDEBUG)
      target_compile_options(
        zevryon-native-damage-presentation-equivalence-tests PRIVATE /UNDEBUG)
    else()
      target_compile_options(
        zevryon-native-damage-presentation-tests PRIVATE -UNDEBUG)
      target_compile_options(
        zevryon-native-damage-presentation-equivalence-tests PRIVATE -UNDEBUG)
    endif()

    add_test(
      NAME native-damage-presentation-tests
      COMMAND zevryon-native-damage-presentation-tests)
    add_test(
      NAME native-damage-presentation-equivalence-tests
      COMMAND zevryon-native-damage-presentation-equivalence-tests)
  endif()
endif()

include("${CMAKE_CURRENT_LIST_DIR}/native_platform_adapters.cmake")
