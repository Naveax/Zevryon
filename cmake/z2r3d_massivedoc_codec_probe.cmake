if(NOT TARGET zevryon-z2r3d-massivedoc-codec-workload)
  add_executable(
    zevryon-z2r3d-massivedoc-codec-workload
    "${CMAKE_CURRENT_LIST_DIR}/../tests/z2r3d_massivedoc_codec_workload.cpp")
  target_include_directories(
    zevryon-z2r3d-massivedoc-codec-workload
    PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../src")
  target_link_libraries(
    zevryon-z2r3d-massivedoc-codec-workload
    PRIVATE zevryon-massivedoc-core)
  set_target_properties(
    zevryon-z2r3d-massivedoc-codec-workload
    PROPERTIES
      CXX_STANDARD 20
      CXX_STANDARD_REQUIRED YES
      CXX_EXTENSIONS NO)
  if(MSVC)
    target_compile_options(
      zevryon-z2r3d-massivedoc-codec-workload
      PRIVATE /W4 /WX /permissive- /EHsc)
  else()
    target_compile_options(
      zevryon-z2r3d-massivedoc-codec-workload
      PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
  endif()
endif()
