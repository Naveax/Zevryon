target_sources(
  zevryon-massivedoc-core
  PRIVATE
    src/full_document_selection.cpp)

if(BUILD_TESTING)
  add_executable(
    zevryon-full-document-selection-tests
    tests/full_document_selection_tests.cpp)
  target_link_libraries(
    zevryon-full-document-selection-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-full-document-selection-tests)
  add_test(
    NAME full-document-selection-tests
    COMMAND zevryon-full-document-selection-tests)
endif()
