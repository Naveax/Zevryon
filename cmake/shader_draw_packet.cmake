add_library(
  zevryon-shader-draw-packet STATIC
  src/shader_draw_packet.cpp)
target_include_directories(zevryon-shader-draw-packet PUBLIC src)
zevryon_options(zevryon-shader-draw-packet)

add_executable(
  zevryon-shader-draw-packet-benchmark
  src/shader_draw_packet_benchmark_main.cpp)
target_include_directories(
  zevryon-shader-draw-packet-benchmark PRIVATE tests)
target_link_libraries(
  zevryon-shader-draw-packet-benchmark
  PRIVATE zevryon-shader-draw-packet)
zevryon_options(zevryon-shader-draw-packet-benchmark)

if(MSVC)
  target_compile_options(
    zevryon-shader-draw-packet-benchmark PRIVATE /UNDEBUG)
else()
  target_compile_options(
    zevryon-shader-draw-packet-benchmark PRIVATE -UNDEBUG)
endif()

if(BUILD_TESTING)
  add_executable(
    zevryon-shader-draw-packet-tests
    tests/shader_draw_packet_tests.cpp)
  target_include_directories(
    zevryon-shader-draw-packet-tests PRIVATE tests)
  target_link_libraries(
    zevryon-shader-draw-packet-tests
    PRIVATE zevryon-shader-draw-packet)
  zevryon_options(zevryon-shader-draw-packet-tests)

  add_executable(
    zevryon-shader-draw-packet-equivalence-tests
    tests/shader_draw_packet_equivalence_tests.cpp)
  target_link_libraries(
    zevryon-shader-draw-packet-equivalence-tests
    PRIVATE zevryon-shader-draw-packet)
  zevryon_options(zevryon-shader-draw-packet-equivalence-tests)

  if(MSVC)
    target_compile_options(
      zevryon-shader-draw-packet-tests PRIVATE /UNDEBUG)
    target_compile_options(
      zevryon-shader-draw-packet-equivalence-tests PRIVATE /UNDEBUG)
  else()
    target_compile_options(
      zevryon-shader-draw-packet-tests PRIVATE -UNDEBUG)
    target_compile_options(
      zevryon-shader-draw-packet-equivalence-tests PRIVATE -UNDEBUG)
  endif()

  add_test(
    NAME shader-draw-packet-tests
    COMMAND zevryon-shader-draw-packet-tests)
  add_test(
    NAME shader-draw-packet-equivalence-tests
    COMMAND zevryon-shader-draw-packet-equivalence-tests)
endif()
