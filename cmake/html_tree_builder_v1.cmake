target_sources(
  zevryon-massivedoc-core
  PRIVATE
    src/html_tree_builder_v1.cpp)

if(BUILD_TESTING)
  add_executable(
    zevryon-html-tree-builder-v1-tests
    tests/html_tree_builder_v1_tests.cpp)
  target_link_libraries(
    zevryon-html-tree-builder-v1-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-html-tree-builder-v1-tests)
  add_test(
    NAME html-tree-builder-v1-tests
    COMMAND zevryon-html-tree-builder-v1-tests)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/z7_wpt_tree_conformance.cmake")
