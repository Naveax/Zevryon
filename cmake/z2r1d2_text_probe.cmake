if(NOT TARGET zevryon-z2r1d2-text-probe)
  add_executable(
    zevryon-z2r1d2-text-probe
    "${CMAKE_CURRENT_LIST_DIR}/../tests/z2r1d2_text_workload_probe_v2.cpp")
  target_include_directories(
    zevryon-z2r1d2-text-probe
    PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../src")
  target_link_libraries(
    zevryon-z2r1d2-text-probe
    PRIVATE zevryon-massivedoc-core)
  set_target_properties(
    zevryon-z2r1d2-text-probe
    PROPERTIES
      CXX_STANDARD 20
      CXX_STANDARD_REQUIRED YES
      CXX_EXTENSIONS NO)
  if(MSVC)
    target_compile_options(
      zevryon-z2r1d2-text-probe
      PRIVATE /W4 /WX /permissive- /EHsc)
  else()
    target_compile_options(
      zevryon-z2r1d2-text-probe
      PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
  endif()
endif()
