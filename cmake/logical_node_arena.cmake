target_sources(
  zevryon-massivedoc-core
  PRIVATE
    src/logical_node_arena.cpp
    src/logical_node_arena_v2.cpp
    src/logical_node_arena_v2_store_bound.cpp
    src/logical_node_record_index.cpp
    src/logical_node_record_index_authoritative.cpp
    src/logical_node_source.cpp
    src/logical_node_source_binding.cpp
    src/logical_node_source_v2.cpp
    src/logical_node_v2_import.cpp
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
endif()
