target_sources(
  zevryon-massivedoc-core
  PRIVATE
    src/html_tokenizer_script_data_v1.cpp)

if(BUILD_TESTING)
  add_executable(
    zevryon-html-tokenizer-script-data-v1-tests
    tests/html_tokenizer_script_data_v1_tests.cpp)
  target_link_libraries(
    zevryon-html-tokenizer-script-data-v1-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-html-tokenizer-script-data-v1-tests)
  add_test(
    NAME html-tokenizer-script-data-v1-tests
    COMMAND zevryon-html-tokenizer-script-data-v1-tests)
endif()
