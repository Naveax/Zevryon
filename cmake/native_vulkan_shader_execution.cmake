if(TARGET zevryon-native-shader-execution AND
   TARGET zevryon-native-gpu-sdk-execution AND
   ZEVRYON_VULKAN_WSI_AVAILABLE)
  find_package(Vulkan QUIET)
  find_package(Python3 QUIET COMPONENTS Interpreter)
  find_program(ZEVRYON_GLSLANG_VALIDATOR glslangValidator)

  if(Vulkan_FOUND AND Python3_Interpreter_FOUND AND ZEVRYON_GLSLANG_VALIDATOR)
    set(
      ZEVRYON_VULKAN_SHADER_SOURCE
      "${CMAKE_CURRENT_SOURCE_DIR}/shaders/native_shader_execution_vulkan.comp")
    set(
      ZEVRYON_VULKAN_SHADER_SPIRV
      "${CMAKE_CURRENT_BINARY_DIR}/generated/native_shader_execution_vulkan.comp.spv")
    set(
      ZEVRYON_VULKAN_SHADER_HEADER
      "${CMAKE_CURRENT_BINARY_DIR}/generated/native_shader_execution_vulkan_spirv.hpp")

    add_custom_command(
      OUTPUT "${ZEVRYON_VULKAN_SHADER_SPIRV}" "${ZEVRYON_VULKAN_SHADER_HEADER}"
      COMMAND ${CMAKE_COMMAND} -E make_directory
              "${CMAKE_CURRENT_BINARY_DIR}/generated"
      COMMAND "${ZEVRYON_GLSLANG_VALIDATOR}"
              -V --target-env vulkan1.0 -S comp
              "${ZEVRYON_VULKAN_SHADER_SOURCE}"
              -o "${ZEVRYON_VULKAN_SHADER_SPIRV}"
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_spirv.py"
              "${ZEVRYON_VULKAN_SHADER_SPIRV}"
              "${ZEVRYON_VULKAN_SHADER_HEADER}"
      DEPENDS
        "${ZEVRYON_VULKAN_SHADER_SOURCE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_spirv.py"
      VERBATIM)

    add_custom_target(
      zevryon-native-shader-execution-vulkan-spirv
      DEPENDS "${ZEVRYON_VULKAN_SHADER_HEADER}")
    add_dependencies(
      zevryon-native-shader-execution
      zevryon-native-shader-execution-vulkan-spirv)
    target_sources(
      zevryon-native-shader-execution
      PRIVATE
        src/native_shader_execution_vulkan.cpp
        "${ZEVRYON_VULKAN_SHADER_HEADER}")
    target_include_directories(
      zevryon-native-shader-execution
      PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_compile_definitions(
      zevryon-native-shader-execution
      PRIVATE ZEVRYON_HAS_VULKAN_SHADER_EXECUTION=1)
    target_link_libraries(
      zevryon-native-shader-execution
      PRIVATE Vulkan::Vulkan zevryon-native-gpu-sdk-execution)

    if(BUILD_TESTING AND TARGET zevryon-vulkan-wsi-test-window)
      add_executable(
        zevryon-native-shader-execution-vulkan-tests
        tests/native_shader_execution_vulkan_tests.cpp)
      target_include_directories(
        zevryon-native-shader-execution-vulkan-tests PRIVATE tests)
      target_link_libraries(
        zevryon-native-shader-execution-vulkan-tests
        PRIVATE
          zevryon-native-shader-execution
          zevryon-vulkan-wsi-test-window)
      zevryon_options(zevryon-native-shader-execution-vulkan-tests)

      add_executable(
        zevryon-native-shader-surface-vulkan-integration-tests
        tests/native_shader_surface_vulkan_integration_tests.cpp)
      target_include_directories(
        zevryon-native-shader-surface-vulkan-integration-tests PRIVATE tests)
      target_link_libraries(
        zevryon-native-shader-surface-vulkan-integration-tests
        PRIVATE
          zevryon-native-shader-execution
          zevryon-vulkan-wsi-test-window)
      zevryon_options(
        zevryon-native-shader-surface-vulkan-integration-tests)

      add_executable(
        zevryon-native-shader-execution-vulkan-benchmark
        src/native_shader_execution_vulkan_benchmark_main.cpp)
      target_include_directories(
        zevryon-native-shader-execution-vulkan-benchmark PRIVATE tests)
      target_link_libraries(
        zevryon-native-shader-execution-vulkan-benchmark
        PRIVATE
          zevryon-native-shader-execution
          zevryon-vulkan-wsi-test-window)
      zevryon_options(zevryon-native-shader-execution-vulkan-benchmark)

      if(MSVC)
        target_compile_options(
          zevryon-native-shader-execution-vulkan-tests PRIVATE /UNDEBUG)
        target_compile_options(
          zevryon-native-shader-surface-vulkan-integration-tests PRIVATE /UNDEBUG)
        target_compile_options(
          zevryon-native-shader-execution-vulkan-benchmark PRIVATE /UNDEBUG)
      else()
        target_compile_options(
          zevryon-native-shader-execution-vulkan-tests PRIVATE -UNDEBUG)
        target_compile_options(
          zevryon-native-shader-surface-vulkan-integration-tests PRIVATE -UNDEBUG)
        target_compile_options(
          zevryon-native-shader-execution-vulkan-benchmark PRIVATE -UNDEBUG)
      endif()

      add_test(
        NAME native-shader-execution-vulkan-tests
        COMMAND zevryon-native-shader-execution-vulkan-tests)
      add_test(
        NAME native-shader-surface-vulkan-integration-tests
        COMMAND zevryon-native-shader-surface-vulkan-integration-tests)
    endif()
  endif()
endif()