include("${CMAKE_CURRENT_LIST_DIR}/font_resource_sfnt.cmake")

option(
  ZEVRYON_ENABLE_CORETEXT_DISCOVERY
  "Build the macOS CoreText discovery adapter"
  ON)

if(ZEVRYON_ENABLE_CORETEXT_DISCOVERY AND APPLE)
  find_library(ZEVRYON_CORETEXT_FRAMEWORK CoreText)
  find_library(ZEVRYON_COREFOUNDATION_FRAMEWORK CoreFoundation)
  if(NOT ZEVRYON_CORETEXT_FRAMEWORK OR NOT ZEVRYON_COREFOUNDATION_FRAMEWORK)
    message(FATAL_ERROR "CoreText and CoreFoundation frameworks are required")
  endif()

  add_library(zevryon-coretext-discovery STATIC src/coretext_discovery.cpp)
  target_include_directories(zevryon-coretext-discovery PUBLIC src)
  target_link_libraries(
    zevryon-coretext-discovery
    PUBLIC zevryon-massivedoc-core
    PRIVATE
      "${ZEVRYON_CORETEXT_FRAMEWORK}"
      "${ZEVRYON_COREFOUNDATION_FRAMEWORK}")
  zevryon_options(zevryon-coretext-discovery)

  if(BUILD_TESTING)
    add_executable(
      zevryon-coretext-discovery-tests
      tests/coretext_discovery_tests.cpp)
    target_link_libraries(
      zevryon-coretext-discovery-tests
      PRIVATE zevryon-coretext-discovery)
    zevryon_options(zevryon-coretext-discovery-tests)
    add_test(
      NAME coretext-discovery-tests
      COMMAND zevryon-coretext-discovery-tests)
  endif()
endif()
