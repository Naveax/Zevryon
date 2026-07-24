option(
  ZEVRYON_ENABLE_HARFBUZZ_SHAPING
  "Build the HarfBuzz shaping backend"
  ON)

if(ZEVRYON_ENABLE_HARFBUZZ_SHAPING)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(HARFBUZZ QUIET IMPORTED_TARGET "harfbuzz>=4.0.0")
  endif()

  if(TARGET PkgConfig::HARFBUZZ)
    add_library(zevryon-harfbuzz-shaper STATIC src/harfbuzz_shaper.cpp)
    target_include_directories(zevryon-harfbuzz-shaper PUBLIC src)
    target_link_libraries(
      zevryon-harfbuzz-shaper
      PUBLIC zevryon-massivedoc-core
      PRIVATE PkgConfig::HARFBUZZ)
    zevryon_options(zevryon-harfbuzz-shaper)

    add_executable(
      zevryon-harfbuzz-shaping-benchmark
      src/harfbuzz_shaping_benchmark_main.cpp)
    target_link_libraries(
      zevryon-harfbuzz-shaping-benchmark
      PRIVATE zevryon-harfbuzz-shaper)
    zevryon_options(zevryon-harfbuzz-shaping-benchmark)

    if(BUILD_TESTING)
      find_file(
        ZEVRYON_TEST_FONT_LATIN
        NAMES DejaVuSans.ttf
        PATH_SUFFIXES
          fonts/truetype/dejavu
          share/fonts/truetype/dejavu
          truetype/dejavu)
      find_file(
        ZEVRYON_TEST_FONT_DEVANAGARI
        NAMES NotoSansDevanagari-Regular.ttf
        PATH_SUFFIXES
          fonts/truetype/noto
          share/fonts/truetype/noto
          truetype/noto)

      add_executable(
        zevryon-harfbuzz-shaper-tests
        tests/harfbuzz_shaper_tests.cpp)
      target_link_libraries(
        zevryon-harfbuzz-shaper-tests
        PRIVATE zevryon-harfbuzz-shaper)
      zevryon_options(zevryon-harfbuzz-shaper-tests)

      if(ZEVRYON_TEST_FONT_LATIN AND ZEVRYON_TEST_FONT_DEVANAGARI)
        add_test(
          NAME harfbuzz-shaper-tests
          COMMAND
            zevryon-harfbuzz-shaper-tests
            "${ZEVRYON_TEST_FONT_LATIN}"
            "${ZEVRYON_TEST_FONT_DEVANAGARI}")
      endif()
    endif()
  endif()
endif()
