if(BUILD_TESTING)
  add_executable(
    zevryon-streaming-html-chunk-size-equivalence-tests
    tests/streaming_html_chunk_size_equivalence_tests.cpp)
  target_link_libraries(
    zevryon-streaming-html-chunk-size-equivalence-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-streaming-html-chunk-size-equivalence-tests)
  add_test(
    NAME z7-chunk-size-equivalence
    COMMAND zevryon-streaming-html-chunk-size-equivalence-tests)

  add_executable(
    zevryon-streaming-html-parser-fuzz-certification-tests
    EXCLUDE_FROM_ALL
    tests/streaming_html_parser_fuzz_certification_tests.cpp)
  target_link_libraries(
    zevryon-streaming-html-parser-fuzz-certification-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-streaming-html-parser-fuzz-certification-tests)

  add_executable(
    zevryon-streaming-html-large-document-certification-tests
    EXCLUDE_FROM_ALL
    tests/streaming_html_large_document_certification_tests.cpp)
  target_link_libraries(
    zevryon-streaming-html-large-document-certification-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-streaming-html-large-document-certification-tests)

  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(
      NAME zenith-program-contract
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/zenith_program_contract.py"
        --manifest "${CMAKE_CURRENT_SOURCE_DIR}/config/zenith_program.json")
  endif()
endif()
