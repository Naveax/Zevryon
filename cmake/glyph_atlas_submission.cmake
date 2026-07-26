if(TARGET zevryon-text-paint-command-stream)
  add_library(
    zevryon-glyph-atlas-submission STATIC
      src/glyph_atlas_submission.cpp)
  target_include_directories(zevryon-glyph-atlas-submission PUBLIC src)
  target_link_libraries(
    zevryon-glyph-atlas-submission
    PUBLIC zevryon-text-paint-command-stream)
  zevryon_options(zevryon-glyph-atlas-submission)

  add_executable(
    zevryon-glyph-atlas-submission-benchmark
    src/glyph_atlas_submission_benchmark_main.cpp)
  target_link_libraries(
    zevryon-glyph-atlas-submission-benchmark
    PRIVATE zevryon-glyph-atlas-submission)
  zevryon_options(zevryon-glyph-atlas-submission-benchmark)

  if(BUILD_TESTING)
    add_executable(
      zevryon-glyph-atlas-submission-tests
      tests/glyph_atlas_submission_tests.cpp)
    target_link_libraries(
      zevryon-glyph-atlas-submission-tests
      PRIVATE zevryon-glyph-atlas-submission)
    zevryon_options(zevryon-glyph-atlas-submission-tests)
    add_test(
      NAME glyph-atlas-submission-tests
      COMMAND zevryon-glyph-atlas-submission-tests)

    add_executable(
      zevryon-glyph-atlas-equivalence-tests
      tests/glyph_atlas_equivalence_tests.cpp)
    target_link_libraries(
      zevryon-glyph-atlas-equivalence-tests
      PRIVATE zevryon-glyph-atlas-submission)
    zevryon_options(zevryon-glyph-atlas-equivalence-tests)
    add_test(
      NAME glyph-atlas-equivalence-tests
      COMMAND zevryon-glyph-atlas-equivalence-tests)
  endif()
endif()
