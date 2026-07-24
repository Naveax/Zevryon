option(
  ZEVRYON_ENABLE_HARFBUZZ_SHAPING
  "Build the HarfBuzz shaping backend"
  ON)

if(ZEVRYON_ENABLE_HARFBUZZ_SHAPING)
  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(HARFBUZZ QUIET IMPORTED_TARGET "harfbuzz>=5.1.0")
  endif()

  if(TARGET PkgConfig::HARFBUZZ)
    # Keep the original HarfBuzz implementation private. The public symbol is
    # provided by harfbuzz_verified_shaper.cpp after font-input selection.
    set_source_files_properties(
      src/harfbuzz_shaper_backend.cpp
      PROPERTIES
        COMPILE_DEFINITIONS
          "shape_harfbuzz_segment=shape_harfbuzz_segment_backend")

    add_library(
      zevryon-harfbuzz-shaper
      STATIC
        src/harfbuzz_shaper_backend.cpp
        src/harfbuzz_verified_shaper.cpp
        src/catalog_harfbuzz_shaper.cpp
        src/prepared_harfbuzz_face.cpp)
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

      add_executable(
        zevryon-harfbuzz-verified-input-tests
        tests/harfbuzz_verified_input_tests.cpp)
      target_link_libraries(
        zevryon-harfbuzz-verified-input-tests
        PRIVATE zevryon-harfbuzz-shaper)
      zevryon_options(zevryon-harfbuzz-verified-input-tests)

      add_executable(
        zevryon-harfbuzz-verified-resource-tests
        tests/harfbuzz_verified_resource_tests.cpp)
      target_link_libraries(
        zevryon-harfbuzz-verified-resource-tests
        PRIVATE zevryon-harfbuzz-shaper)
      zevryon_options(zevryon-harfbuzz-verified-resource-tests)

      add_executable(
        zevryon-catalog-harfbuzz-shaper-tests
        tests/catalog_harfbuzz_shaper_tests.cpp)
      target_link_libraries(
        zevryon-catalog-harfbuzz-shaper-tests
        PRIVATE zevryon-harfbuzz-shaper)
      zevryon_options(zevryon-catalog-harfbuzz-shaper-tests)

      add_executable(
        zevryon-prepared-harfbuzz-face-tests
        tests/prepared_harfbuzz_face_tests.cpp)
      target_link_libraries(
        zevryon-prepared-harfbuzz-face-tests
        PRIVATE zevryon-harfbuzz-shaper)
      zevryon_options(zevryon-prepared-harfbuzz-face-tests)

      add_executable(
        zevryon-prepared-harfbuzz-shaping-tests
        tests/prepared_harfbuzz_shaping_tests.cpp)
      target_link_libraries(
        zevryon-prepared-harfbuzz-shaping-tests
        PRIVATE zevryon-harfbuzz-shaper)
      zevryon_options(zevryon-prepared-harfbuzz-shaping-tests)

      if(ZEVRYON_TEST_FONT_LATIN)
        add_test(
          NAME catalog-harfbuzz-shaper-tests
          COMMAND
            zevryon-catalog-harfbuzz-shaper-tests
            "${ZEVRYON_TEST_FONT_LATIN}")
        add_test(
          NAME prepared-harfbuzz-face-tests
          COMMAND
            zevryon-prepared-harfbuzz-face-tests
            "${ZEVRYON_TEST_FONT_LATIN}")
        add_test(
          NAME prepared-harfbuzz-shaping-tests
          COMMAND
            zevryon-prepared-harfbuzz-shaping-tests
            "${ZEVRYON_TEST_FONT_LATIN}")
      endif()

      if(ZEVRYON_TEST_FONT_LATIN AND ZEVRYON_TEST_FONT_DEVANAGARI)
        add_test(
          NAME harfbuzz-shaper-tests
          COMMAND
            zevryon-harfbuzz-shaper-tests
            "${ZEVRYON_TEST_FONT_LATIN}"
            "${ZEVRYON_TEST_FONT_DEVANAGARI}")
        add_test(
          NAME harfbuzz-verified-input-tests
          COMMAND
            zevryon-harfbuzz-verified-input-tests
            "${ZEVRYON_TEST_FONT_LATIN}")
        add_test(
          NAME harfbuzz-verified-resource-tests
          COMMAND
            zevryon-harfbuzz-verified-resource-tests
            "${ZEVRYON_TEST_FONT_LATIN}")
      endif()
    endif()
  endif()
endif()
