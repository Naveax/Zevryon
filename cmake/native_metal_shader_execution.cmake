if(APPLE AND
   TARGET zevryon-native-shader-execution AND
   TARGET zevryon-native-gpu-sdk-execution)
  find_package(Python3 QUIET COMPONENTS Interpreter)
  find_program(ZEVRYON_XCRUN xcrun)

  if(Python3_Interpreter_FOUND AND ZEVRYON_XCRUN)
    set(
      ZEVRYON_METAL_SHADER_SOURCE
      "${CMAKE_CURRENT_SOURCE_DIR}/shaders/native_shader_execution_metal.metal")
    set(
      ZEVRYON_METAL_SHADER_AIR
      "${CMAKE_CURRENT_BINARY_DIR}/generated/native_shader_execution_metal.air")
    set(
      ZEVRYON_METAL_SHADER_LIBRARY
      "${CMAKE_CURRENT_BINARY_DIR}/generated/native_shader_execution_metal.metallib")
    set(
      ZEVRYON_METAL_SHADER_HEADER
      "${CMAKE_CURRENT_BINARY_DIR}/generated/native_shader_execution_metal_metallib.hpp")

    add_custom_command(
      OUTPUT
        "${ZEVRYON_METAL_SHADER_AIR}"
        "${ZEVRYON_METAL_SHADER_LIBRARY}"
        "${ZEVRYON_METAL_SHADER_HEADER}"
      COMMAND ${CMAKE_COMMAND} -E make_directory
              "${CMAKE_CURRENT_BINARY_DIR}/generated"
      COMMAND "${ZEVRYON_XCRUN}" -sdk macosx metal
              -c
              "${ZEVRYON_METAL_SHADER_SOURCE}"
              -o "${ZEVRYON_METAL_SHADER_AIR}"
      COMMAND "${ZEVRYON_XCRUN}" -sdk macosx metallib
              "${ZEVRYON_METAL_SHADER_AIR}"
              -o "${ZEVRYON_METAL_SHADER_LIBRARY}"
      COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_binary.py"
              "${ZEVRYON_METAL_SHADER_LIBRARY}"
              "${ZEVRYON_METAL_SHADER_HEADER}"
              kMetalIntegerComposerMetallib
      DEPENDS
        "${ZEVRYON_METAL_SHADER_SOURCE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_binary.py"
      VERBATIM)

    add_custom_target(
      zevryon-native-shader-execution-metal-metallib
      DEPENDS "${ZEVRYON_METAL_SHADER_HEADER}")
    add_dependencies(
      zevryon-native-shader-execution
      zevryon-native-shader-execution-metal-metallib)
    target_sources(
      zevryon-native-shader-execution
      PRIVATE
        src/native_shader_execution_metal.mm
        "${ZEVRYON_METAL_SHADER_HEADER}")
    set_source_files_properties(
      src/native_shader_execution_metal.mm
      PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    target_include_directories(
      zevryon-native-shader-execution
      PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_compile_definitions(
      zevryon-native-shader-execution
      PRIVATE ZEVRYON_HAS_METAL_SHADER_EXECUTION=1)
    target_link_libraries(
      zevryon-native-shader-execution
      PRIVATE
        zevryon-native-gpu-sdk-execution
        "-framework AppKit"
        "-framework Foundation"
        "-framework Metal"
        "-framework QuartzCore")

    if(BUILD_TESTING AND TARGET zevryon-metal-window-test-host)
      add_executable(
        zevryon-native-shader-execution-metal-tests
        tests/native_shader_execution_metal_tests.cpp)
      target_include_directories(
        zevryon-native-shader-execution-metal-tests PRIVATE tests)
      target_link_libraries(
        zevryon-native-shader-execution-metal-tests
        PRIVATE
          zevryon-native-shader-execution
          zevryon-metal-window-test-host)
      zevryon_options(zevryon-native-shader-execution-metal-tests)

      add_executable(
        zevryon-native-shader-execution-metal-benchmark
        src/native_shader_execution_metal_benchmark_main.cpp)
      target_include_directories(
        zevryon-native-shader-execution-metal-benchmark PRIVATE tests)
      target_link_libraries(
        zevryon-native-shader-execution-metal-benchmark
        PRIVATE
          zevryon-native-shader-execution
          zevryon-metal-window-test-host)
      zevryon_options(zevryon-native-shader-execution-metal-benchmark)

      target_compile_options(
        zevryon-native-shader-execution-metal-tests PRIVATE -UNDEBUG)
      target_compile_options(
        zevryon-native-shader-execution-metal-benchmark PRIVATE -UNDEBUG)

      add_test(
        NAME native-shader-execution-metal-tests
        COMMAND zevryon-native-shader-execution-metal-tests)
    endif()
  endif()
endif()
