if(TARGET zevryon-viewport-projection)
  add_library(
    zevryon-text-paint-command-stream STATIC
      src/text_paint_command_stream.cpp)
  target_include_directories(zevryon-text-paint-command-stream PUBLIC src)
  target_link_libraries(
    zevryon-text-paint-command-stream
    PUBLIC zevryon-viewport-projection)
  zevryon_options(zevryon-text-paint-command-stream)

  add_executable(
    zevryon-text-paint-command-stream-benchmark
    src/text_paint_command_stream_benchmark_main.cpp)
  target_link_libraries(
    zevryon-text-paint-command-stream-benchmark
    PRIVATE zevryon-text-paint-command-stream)
  zevryon_options(zevryon-text-paint-command-stream-benchmark)

  if(BUILD_TESTING)
    add_executable(
      zevryon-text-paint-command-stream-tests
      tests/text_paint_command_stream_tests.cpp)
    target_link_libraries(
      zevryon-text-paint-command-stream-tests
      PRIVATE zevryon-text-paint-command-stream)
    zevryon_options(zevryon-text-paint-command-stream-tests)
    add_test(
      NAME text-paint-command-stream-tests
      COMMAND zevryon-text-paint-command-stream-tests)

    add_executable(
      zevryon-text-paint-command-equivalence-tests
      tests/text_paint_command_equivalence_tests.cpp)
    target_link_libraries(
      zevryon-text-paint-command-equivalence-tests
      PRIVATE zevryon-text-paint-command-stream)
    zevryon_options(zevryon-text-paint-command-equivalence-tests)
    add_test(
      NAME text-paint-command-equivalence-tests
      COMMAND zevryon-text-paint-command-equivalence-tests)
  endif()
endif()
