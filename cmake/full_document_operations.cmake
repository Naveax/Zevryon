target_sources(
  zevryon-massivedoc-core
  PRIVATE
    src/full_document_selection.cpp
    src/full_document_export.cpp)

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

  add_executable(
    zevryon-full-document-export-tests
    tests/full_document_export_tests.cpp)
  target_link_libraries(
    zevryon-full-document-export-tests
    PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-full-document-export-tests)
  add_test(
    NAME full-document-export-tests
    COMMAND zevryon-full-document-export-tests)
endif()
