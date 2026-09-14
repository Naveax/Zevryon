if(BUILD_TESTING)
  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(
      NAME z7-wpt-tree-structural-census-v1
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/z7_wpt_tree_structural_census_v1.py"
        --probe "$<TARGET_FILE:zevryon-html-tree-dump-v1-probe>")
    add_test(
      NAME z7-wpt-tree-structural-census-v1-self-test
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/z7_wpt_tree_structural_census_v1.py"
        --self-test)
  endif()
endif()
