target_sources(
  zevryon-massivedoc-core
  PRIVATE
    src/logical_node_arena.cpp)

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
endif()
