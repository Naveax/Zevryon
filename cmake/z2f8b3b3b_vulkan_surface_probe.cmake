if(NOT TARGET zevryon-z2f8b3b3b-vulkan-surface-contract)
  find_package(Vulkan REQUIRED)

  add_executable(
    zevryon-z2f8b3b3b-vulkan-surface-contract
    "${CMAKE_CURRENT_LIST_DIR}/../src/native_shader_surface_vulkan.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../tests/native_shader_surface_vulkan_contract_tests.cpp")
  target_include_directories(
    zevryon-z2f8b3b3b-vulkan-surface-contract
    PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../src")
  target_compile_definitions(
    zevryon-z2f8b3b3b-vulkan-surface-contract
    PRIVATE ZEVRYON_HAS_VULKAN_WSI=1)
  target_link_libraries(
    zevryon-z2f8b3b3b-vulkan-surface-contract
    PRIVATE Vulkan::Vulkan)
  set_target_properties(
    zevryon-z2f8b3b3b-vulkan-surface-contract
    PROPERTIES
      CXX_STANDARD 20
      CXX_STANDARD_REQUIRED YES
      CXX_EXTENSIONS NO)

  if(MSVC)
    target_compile_options(
      zevryon-z2f8b3b3b-vulkan-surface-contract
      PRIVATE /W4 /WX /permissive- /EHsc /UNDEBUG)
  else()
    target_compile_options(
      zevryon-z2f8b3b3b-vulkan-surface-contract
      PRIVATE -Wall -Wextra -Wpedantic -Werror -UNDEBUG)
  endif()

  enable_testing()
  add_test(
    NAME z2f8b3b3b-vulkan-surface-contract
    COMMAND zevryon-z2f8b3b3b-vulkan-surface-contract)
endif()
