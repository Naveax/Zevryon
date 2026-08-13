if(NOT TARGET zevryon-z2r1d4-platform-probe)
  if(NOT WIN32)
    message(FATAL_ERROR "Z2R-1D4 platform probe supports Windows only")
  endif()

  add_executable(
    zevryon-z2r1d4-platform-probe
    "${CMAKE_CURRENT_LIST_DIR}/../tests/z2r1d4_platform_discovery_probe.cpp")
  target_include_directories(
    zevryon-z2r1d4-platform-probe
    PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../src")
  target_link_libraries(
    zevryon-z2r1d4-platform-probe
    PRIVATE zevryon-directwrite-discovery)
  set_target_properties(
    zevryon-z2r1d4-platform-probe
    PROPERTIES
      CXX_STANDARD 20
      CXX_STANDARD_REQUIRED YES
      CXX_EXTENSIONS NO)
  if(MSVC)
    target_compile_options(
      zevryon-z2r1d4-platform-probe
      PRIVATE /W4 /WX /permissive- /EHsc)
  else()
    target_compile_options(
      zevryon-z2r1d4-platform-probe
      PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
  endif()
endif()
