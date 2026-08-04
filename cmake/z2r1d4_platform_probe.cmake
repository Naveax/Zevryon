if(NOT TARGET zevryon-z2r1d4-platform-probe)
  add_executable(
    zevryon-z2r1d4-platform-probe
    "${CMAKE_CURRENT_LIST_DIR}/../tests/z2r1d4_platform_discovery_probe.cpp")
  target_include_directories(
    zevryon-z2r1d4-platform-probe
    PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../src")
  if(WIN32)
    target_link_libraries(
      zevryon-z2r1d4-platform-probe
      PRIVATE zevryon-directwrite-discovery)
  elseif(APPLE)
    target_link_libraries(
      zevryon-z2r1d4-platform-probe
      PRIVATE zevryon-coretext-discovery)
  else()
    message(FATAL_ERROR "Z2R-1D4 platform probe supports Windows and macOS only")
  endif()
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
