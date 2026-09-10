if(BUILD_TESTING)
  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(
      NAME z7-html-named-reference-table-provenance
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/z7_html_named_reference_table_verify.py")
    add_test(
      NAME z7-html-named-reference-table-verifier-tests
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/z7_html_named_reference_table_verify.py"
        --self-test)
  endif()
endif()
