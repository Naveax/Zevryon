if(NOT TARGET zevryon-z2r3du-unicode-workload)
  add_executable(
    zevryon-z2r3du-unicode-workload
    "${CMAKE_CURRENT_LIST_DIR}/../tests/z2r3d_unicode_workload.cpp")
  target_include_directories(
    zevryon-z2r3du-unicode-workload
    PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../src")
  target_link_libraries(
    zevryon-z2r3du-unicode-workload
    PRIVATE zevryon-massivedoc-core)
  set_target_properties(
    zevryon-z2r3du-unicode-workload
    PROPERTIES
      CXX_STANDARD 20
      CXX_STANDARD_REQUIRED YES
      CXX_EXTENSIONS NO)
  if(MSVC)
    target_compile_options(
      zevryon-z2r3du-unicode-workload
      PRIVATE /W4 /WX /permissive- /EHsc)
  else()
    target_compile_options(
      zevryon-z2r3du-unicode-workload
      PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endif()
