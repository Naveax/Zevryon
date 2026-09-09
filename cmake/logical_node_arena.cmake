target_sources(
  zevryon-massivedoc-core
  PRIVATE
    src/html_tokenizer_data_tags_v1.cpp
    src/html_tokenizer_markup_declarations_v1.cpp
    src/html_tokenizer_token_stream_v1.cpp
    src/logical_node_arena.cpp
    src/logical_node_arena_v2.cpp
    src/logical_node_arena_v2_store_bound.cpp
    src/logical_node_record_index_semantic_adopted.cpp
    src/logical_node_record_index_authoritative_adopted.cpp
    src/logical_node_source.cpp
    src/logical_node_source_binding.cpp
    src/logical_node_source_v2.cpp
    src/logical_node_v2_import.cpp
    src/streaming_html_node_arena_v2.cpp
    src/streaming_html_node_source.cpp
    src/streaming_html_node_source_v2.cpp
    src/zenith_semantic_node_window.cpp
    src/zenith_semantic_runtime_consumer.cpp)

if(BUILD_TESTING)
  add_executable(
    zevryon-logical-node-arena-tests
    tests/logical_node_arena_tests.cpp)
  target_link_libraries(
    zevryon-logical-node-arena-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-logical-node-arena-tests)
  add_test(
    NAME logical-node-arena-tests
    COMMAND zevryon-logical-node-arena-tests)

  add_executable(
    zevryon-logical-node-arena-v2-tests
    tests/logical_node_arena_v2_tests.cpp)
  target_link_libraries(
    zevryon-logical-node-arena-v2-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-logical-node-arena-v2-tests)
  add_test(
    NAME logical-node-arena-v2-tests
    COMMAND zevryon-logical-node-arena-v2-tests)

  add_executable(
    zevryon-logical-node-source-tests
    tests/logical_node_source_tests.cpp)
  target_link_libraries(
    zevryon-logical-node-source-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-logical-node-source-tests)
  add_test(
    NAME logical-node-source-tests
    COMMAND zevryon-logical-node-source-tests)

  add_executable(
    zevryon-logical-node-source-v2-tests
    tests/logical_node_source_v2_tests.cpp)
  target_link_libraries(
    zevryon-logical-node-source-v2-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-logical-node-source-v2-tests)
  add_test(
    NAME logical-node-source-v2-tests
    COMMAND zevryon-logical-node-source-v2-tests)

  add_executable(
    zevryon-logical-node-v2-import-tests
    tests/logical_node_v2_import_tests.cpp)
  target_link_libraries(
    zevryon-logical-node-v2-import-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-logical-node-v2-import-tests)
  add_test(
    NAME logical-node-v2-import-tests
    COMMAND zevryon-logical-node-v2-import-tests)

  add_executable(
    zevryon-streaming-html-node-source-tests
    tests/streaming_html_node_source_tests.cpp)
  target_link_libraries(
    zevryon-streaming-html-node-source-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-streaming-html-node-source-tests)
  add_test(
    NAME streaming-html-node-source-tests
    COMMAND zevryon-streaming-html-node-source-tests)

  add_executable(
    zevryon-streaming-html-node-source-v2-tests
    tests/streaming_html_node_source_v2_tests.cpp)
  target_link_libraries(
    zevryon-streaming-html-node-source-v2-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-streaming-html-node-source-v2-tests)
  add_test(
    NAME streaming-html-node-source-v2-tests
    COMMAND zevryon-streaming-html-node-source-v2-tests)

  add_executable(
    zevryon-streaming-html-node-source-v2-strict-tests
    tests/streaming_html_node_source_v2_strict_tests.cpp)
  target_link_libraries(
    zevryon-streaming-html-node-source-v2-strict-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-streaming-html-node-source-v2-strict-tests)
  add_test(
    NAME streaming-html-node-source-v2-strict-tests
    COMMAND zevryon-streaming-html-node-source-v2-strict-tests)

  add_executable(
    zevryon-html-tokenizer-token-stream-v1-tests
    tests/html_tokenizer_token_stream_v1_tests.cpp)
  target_link_libraries(
    zevryon-html-tokenizer-token-stream-v1-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-html-tokenizer-token-stream-v1-tests)
  add_test(
    NAME html-tokenizer-token-stream-v1-tests
    COMMAND zevryon-html-tokenizer-token-stream-v1-tests)

  add_executable(
    zevryon-html-tokenizer-data-tags-v1-tests
    tests/html_tokenizer_data_tags_v1_tests.cpp)
  target_link_libraries(
    zevryon-html-tokenizer-data-tags-v1-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-html-tokenizer-data-tags-v1-tests)
  add_test(
    NAME html-tokenizer-data-tags-v1-tests
    COMMAND zevryon-html-tokenizer-data-tags-v1-tests)

  add_executable(
    zevryon-html-tokenizer-markup-declarations-v1-tests
    tests/html_tokenizer_markup_declarations_v1_tests.cpp)
  target_link_libraries(
    zevryon-html-tokenizer-markup-declarations-v1-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-html-tokenizer-markup-declarations-v1-tests)
  add_test(
    NAME html-tokenizer-markup-declarations-v1-tests
    COMMAND zevryon-html-tokenizer-markup-declarations-v1-tests)

  add_executable(
    zevryon-html-tokenizer-token-stream-v1-probe
    tests/html_tokenizer_token_stream_v1_probe.cpp)
  target_link_libraries(
    zevryon-html-tokenizer-token-stream-v1-probe
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-html-tokenizer-token-stream-v1-probe)

  add_executable(
    zevryon-streaming-html-node-source-v2-property-fuzz-tests
    tests/streaming_html_node_source_v2_property_fuzz_tests.cpp)
  target_link_libraries(
    zevryon-streaming-html-node-source-v2-property-fuzz-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-streaming-html-node-source-v2-property-fuzz-tests)
  add_test(
    NAME streaming-html-node-source-v2-property-fuzz-tests
    COMMAND zevryon-streaming-html-node-source-v2-property-fuzz-tests)

  add_executable(
    zevryon-streaming-html-node-source-v2-large-document-tests
    tests/streaming_html_node_source_v2_large_document_tests.cpp)
  target_link_libraries(
    zevryon-streaming-html-node-source-v2-large-document-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-streaming-html-node-source-v2-large-document-tests)
  add_test(
    NAME streaming-html-node-source-v2-large-document-tests
    COMMAND zevryon-streaming-html-node-source-v2-large-document-tests)

  add_executable(
    zevryon-streaming-html-node-arena-v2-tests
    tests/streaming_html_node_arena_v2_tests.cpp)
  target_link_libraries(
    zevryon-streaming-html-node-arena-v2-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-streaming-html-node-arena-v2-tests)
  add_test(
    NAME streaming-html-node-arena-v2-tests
    COMMAND zevryon-streaming-html-node-arena-v2-tests)

  add_executable(
    zevryon-streaming-html-resource-accounting-tests
    tests/streaming_html_resource_accounting_tests.cpp)
  target_link_libraries(
    zevryon-streaming-html-resource-accounting-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-streaming-html-resource-accounting-tests)
  add_test(
    NAME streaming-html-resource-accounting-tests
    COMMAND zevryon-streaming-html-resource-accounting-tests)

  add_executable(
    zevryon-logical-node-record-index-tests
    tests/logical_node_record_index_tests.cpp)
  target_link_libraries(
    zevryon-logical-node-record-index-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-logical-node-record-index-tests)
  add_test(
    NAME logical-node-record-index-tests
    COMMAND zevryon-logical-node-record-index-tests)

  add_executable(
    zevryon-logical-node-record-index-corruption-tests
    tests/logical_node_record_index_corruption_tests.cpp)
  target_link_libraries(
    zevryon-logical-node-record-index-corruption-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-logical-node-record-index-corruption-tests)
  add_test(
    NAME logical-node-record-index-corruption-tests
    COMMAND zevryon-logical-node-record-index-corruption-tests)

  add_executable(
    zevryon-logical-node-record-index-authority-tests
    tests/logical_node_record_index_authority_tests.cpp)
  target_link_libraries(
    zevryon-logical-node-record-index-authority-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-logical-node-record-index-authority-tests)
  add_test(
    NAME logical-node-record-index-authority-tests
    COMMAND zevryon-logical-node-record-index-authority-tests)

  add_executable(
    zevryon-zenith-semantic-node-window-tests
    tests/zenith_semantic_node_window_tests.cpp)
  target_link_libraries(
    zevryon-zenith-semantic-node-window-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-zenith-semantic-node-window-tests)
  add_test(
    NAME zenith-semantic-node-window-tests
    COMMAND zevryon-zenith-semantic-node-window-tests)

  add_executable(
    zevryon-zenith-semantic-runtime-consumer-tests
    tests/zenith_semantic_runtime_consumer_tests.cpp)
  target_link_libraries(
    zevryon-zenith-semantic-runtime-consumer-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-zenith-semantic-runtime-consumer-tests)
  add_test(
    NAME zenith-semantic-runtime-consumer-tests
    COMMAND zevryon-zenith-semantic-runtime-consumer-tests)

  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(
      NAME z7-html5lib-tokenizer-corpus-provenance
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/z7_html5lib_tokenizer_corpus_verify.py")
    add_test(
      NAME z7-html5lib-tokenizer-corpus-verifier-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/z7_html5lib_tokenizer_corpus_verify.py"
        --self-test)
    add_test(
      NAME z7-html5lib-tokenizer-runner-v1
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/z7_html5lib_tokenizer_runner_v1.py"
        --probe "$<TARGET_FILE:zevryon-html-tokenizer-token-stream-v1-probe>")
    add_test(
      NAME z7-wpt-tree-corpus-provenance
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/z7_wpt_tree_corpus_verify.py")
    add_test(
      NAME z7-wpt-tree-corpus-verifier-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/z7_wpt_tree_corpus_verify.py"
        --self-test)
  endif()
endif()
