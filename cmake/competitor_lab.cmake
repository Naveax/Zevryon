if(BUILD_TESTING)
  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(
      NAME m7-competitor-lab-contract-smoke
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/competitor_lab_smoke.py")
    add_test(
      NAME m7-competitor-lab-workload-identity-smoke
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/competitor_lab_v2_smoke.py")
    add_test(
      NAME m7-competitor-adapter-protocol-smoke
      COMMAND
        "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/competitor_adapter_smoke.py")
  endif()
endif()
