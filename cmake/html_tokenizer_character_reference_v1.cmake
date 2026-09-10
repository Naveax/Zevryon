target_sources(
  zevryon-massivedoc-core
  PRIVATE
    src/html_tokenizer_character_reference_v1.cpp)

if(BUILD_TESTING)
  add_executable(
    zevryon-html-tokenizer-numeric-character-reference-v1-tests
    tests/html_tokenizer_numeric_character_reference_v1_tests.cpp)
  target_link_libraries(
    zevryon-html-tokenizer-numeric-character-reference-v1-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-html-tokenizer-numeric-character-reference-v1-tests)
  add_test(
    NAME html-tokenizer-numeric-character-reference-v1-tests
    COMMAND zevryon-html-tokenizer-numeric-character-reference-v1-tests)
endif()
