if(TARGET zevryon-shader-draw-packet AND
   TARGET zevryon-native-gpu-sdk-execution)
  set(
    ZEVRYON_NATIVE_SHADER_EXECUTION_SOURCES
    src/native_shader_execution.cpp)
  set(ZEVRYON_NATIVE_SHADER_EXECUTION_DEFINITIONS)
  set(ZEVRYON_NATIVE_SHADER_EXECUTION_LIBRARIES)

  if(WIN32)
    list(APPEND
      ZEVRYON_NATIVE_SHADER_EXECUTION_SOURCES
      src/native_shader_execution_d3d12.cpp)
    list(APPEND
      ZEVRYON_NATIVE_SHADER_EXECUTION_DEFINITIONS
      ZEVRYON_HAS_D3D12_NATIVE_SHADER=1)
    list(APPEND
      ZEVRYON_NATIVE_SHADER_EXECUTION_LIBRARIES
      d3d12 d3dcompiler dxgi dxguid user32)
  endif()

  if(APPLE)
    enable_language(OBJCXX)
    list(APPEND
      ZEVRYON_NATIVE_SHADER_EXECUTION_SOURCES
      src/native_shader_execution_metal.mm)
    list(APPEND
      ZEVRYON_NATIVE_SHADER_EXECUTION_DEFINITIONS
      ZEVRYON_HAS_METAL_NATIVE_SHADER=1)
    list(APPEND
      ZEVRYON_NATIVE_SHADER_EXECUTION_LIBRARIES
      "-framework AppKit"
      "-framework Foundation"
      "-framework Metal"
      "-framework QuartzCore")
    set_source_files_properties(
      src/native_shader_execution_metal.mm
      PROPERTIES COMPILE_FLAGS "-fobjc-arc")
  endif()

  if(Vulkan_FOUND AND ZEVRYON_VULKAN_WSI_AVAILABLE)
    list(APPEND
      ZEVRYON_NATIVE_SHADER_EXECUTION_SOURCES
      src/native_shader_execution_vulkan.cpp)
    list(APPEND
      ZEVRYON_NATIVE_SHADER_EXECUTION_DEFINITIONS
      ZEVRYON_HAS_VULKAN_NATIVE_SHADER=1)
    list(APPEND
      ZEVRYON_NATIVE_SHADER_EXECUTION_LIBRARIES
      Vulkan::Vulkan)
  endif()

  add_library(
    zevryon-native-shader-execution STATIC
    ${ZEVRYON_NATIVE_SHADER_EXECUTION_SOURCES})
  target_include_directories(
    zevryon-native-shader-execution PUBLIC src)
  target_link_libraries(
    zevryon-native-shader-execution
    PUBLIC
      zevryon-shader-draw-packet
      zevryon-native-gpu-sdk-execution
    PRIVATE
      ${ZEVRYON_NATIVE_SHADER_EXECUTION_LIBRARIES})
  if(ZEVRYON_NATIVE_SHADER_EXECUTION_DEFINITIONS)
    target_compile_definitions(
      zevryon-native-shader-execution
      PRIVATE ${ZEVRYON_NATIVE_SHADER_EXECUTION_DEFINITIONS})
  endif()
  zevryon_options(zevryon-native-shader-execution)

  add_executable(
    zevryon-native-shader-execution-benchmark
    src/native_shader_execution_benchmark_main.cpp)
  target_include_directories(
    zevryon-native-shader-execution-benchmark PRIVATE tests)
  target_link_libraries(
    zevryon-native-shader-execution-benchmark
    PRIVATE zevryon-native-shader-execution)
  zevryon_options(zevryon-native-shader-execution-benchmark)

  if(MSVC)
    target_compile_options(
      zevryon-native-shader-execution-benchmark PRIVATE /UNDEBUG)
  else()
    target_compile_options(
      zevryon-native-shader-execution-benchmark PRIVATE -UNDEBUG)
  endif()

  if(BUILD_TESTING)
    add_executable(
      zevryon-native-shader-execution-tests
      tests/native_shader_execution_tests.cpp)
    target_include_directories(
      zevryon-native-shader-execution-tests PRIVATE tests)
    target_link_libraries(
      zevryon-native-shader-execution-tests
      PRIVATE zevryon-native-shader-execution)
    zevryon_options(zevryon-native-shader-execution-tests)

    add_executable(
      zevryon-native-shader-execution-equivalence-tests
      tests/native_shader_execution_equivalence_tests.cpp)
    target_include_directories(
      zevryon-native-shader-execution-equivalence-tests PRIVATE tests)
    target_link_libraries(
      zevryon-native-shader-execution-equivalence-tests
      PRIVATE zevryon-native-shader-execution)
    zevryon_options(zevryon-native-shader-execution-equivalence-tests)

    if(WIN32)
      add_executable(
        zevryon-native-shader-execution-d3d12-tests
        tests/native_shader_execution_platform_tests.cpp)
      target_include_directories(
        zevryon-native-shader-execution-d3d12-tests PRIVATE tests)
      target_compile_definitions(
        zevryon-native-shader-execution-d3d12-tests
        PRIVATE ZEVRYON_NATIVE_SHADER_TEST_D3D12=1)
      target_link_libraries(
        zevryon-native-shader-execution-d3d12-tests
        PRIVATE zevryon-native-shader-execution)
      zevryon_options(zevryon-native-shader-execution-d3d12-tests)

      add_executable(
        zevryon-native-shader-execution-d3d12-benchmark
        src/native_shader_execution_platform_benchmark_main.cpp)
      target_include_directories(
        zevryon-native-shader-execution-d3d12-benchmark PRIVATE tests)
      target_compile_definitions(
        zevryon-native-shader-execution-d3d12-benchmark
        PRIVATE ZEVRYON_NATIVE_SHADER_TEST_D3D12=1)
      target_link_libraries(
        zevryon-native-shader-execution-d3d12-benchmark
        PRIVATE zevryon-native-shader-execution)
      zevryon_options(zevryon-native-shader-execution-d3d12-benchmark)
    endif()

    if(APPLE AND TARGET zevryon-metal-window-test-host)
      add_executable(
        zevryon-native-shader-execution-metal-tests
        tests/native_shader_execution_platform_tests.cpp)
      target_include_directories(
        zevryon-native-shader-execution-metal-tests PRIVATE tests)
      target_compile_definitions(
        zevryon-native-shader-execution-metal-tests
        PRIVATE ZEVRYON_NATIVE_SHADER_TEST_METAL=1)
      target_link_libraries(
        zevryon-native-shader-execution-metal-tests
        PRIVATE
          zevryon-native-shader-execution
          zevryon-metal-window-test-host)
      zevryon_options(zevryon-native-shader-execution-metal-tests)

      add_executable(
        zevryon-native-shader-execution-metal-benchmark
        src/native_shader_execution_platform_benchmark_main.cpp)
      target_include_directories(
        zevryon-native-shader-execution-metal-benchmark PRIVATE tests)
      target_compile_definitions(
        zevryon-native-shader-execution-metal-benchmark
        PRIVATE ZEVRYON_NATIVE_SHADER_TEST_METAL=1)
      target_link_libraries(
        zevryon-native-shader-execution-metal-benchmark
        PRIVATE
          zevryon-native-shader-execution
          zevryon-metal-window-test-host)
      zevryon_options(zevryon-native-shader-execution-metal-benchmark)
    endif()

    if(Vulkan_FOUND AND TARGET zevryon-vulkan-wsi-test-window)
      add_executable(
        zevryon-native-shader-execution-vulkan-tests
        tests/native_shader_execution_platform_tests.cpp)
      target_include_directories(
        zevryon-native-shader-execution-vulkan-tests PRIVATE tests)
      target_compile_definitions(
        zevryon-native-shader-execution-vulkan-tests
        PRIVATE ZEVRYON_NATIVE_SHADER_TEST_VULKAN=1)
      if(ZEVRYON_XCB_FOUND)
        target_compile_definitions(
          zevryon-native-shader-execution-vulkan-tests
          PRIVATE ZEVRYON_VULKAN_WSI_TEST_XCB=1)
      elseif(ZEVRYON_WAYLAND_FOUND)
        target_compile_definitions(
          zevryon-native-shader-execution-vulkan-tests
          PRIVATE ZEVRYON_VULKAN_WSI_TEST_WAYLAND=1)
      endif()
      target_link_libraries(
        zevryon-native-shader-execution-vulkan-tests
        PRIVATE
          zevryon-native-shader-execution
          zevryon-vulkan-wsi-test-window)
      zevryon_options(zevryon-native-shader-execution-vulkan-tests)

      add_executable(
        zevryon-native-shader-execution-vulkan-benchmark
        src/native_shader_execution_platform_benchmark_main.cpp)
      target_include_directories(
        zevryon-native-shader-execution-vulkan-benchmark PRIVATE tests)
      target_compile_definitions(
        zevryon-native-shader-execution-vulkan-benchmark
        PRIVATE ZEVRYON_NATIVE_SHADER_TEST_VULKAN=1)
      if(ZEVRYON_XCB_FOUND)
        target_compile_definitions(
          zevryon-native-shader-execution-vulkan-benchmark
          PRIVATE ZEVRYON_VULKAN_WSI_TEST_XCB=1)
      elseif(ZEVRYON_WAYLAND_FOUND)
        target_compile_definitions(
          zevryon-native-shader-execution-vulkan-benchmark
          PRIVATE ZEVRYON_VULKAN_WSI_TEST_WAYLAND=1)
      endif()
      target_link_libraries(
        zevryon-native-shader-execution-vulkan-benchmark
        PRIVATE
          zevryon-native-shader-execution
          zevryon-vulkan-wsi-test-window)
      zevryon_options(zevryon-native-shader-execution-vulkan-benchmark)
    endif()

    if(MSVC)
      target_compile_options(
        zevryon-native-shader-execution-tests PRIVATE /UNDEBUG)
      target_compile_options(
        zevryon-native-shader-execution-equivalence-tests PRIVATE /UNDEBUG)
      if(TARGET zevryon-native-shader-execution-d3d12-tests)
        target_compile_options(
          zevryon-native-shader-execution-d3d12-tests PRIVATE /UNDEBUG)
        target_compile_options(
          zevryon-native-shader-execution-d3d12-benchmark PRIVATE /UNDEBUG)
      endif()
    else()
      target_compile_options(
        zevryon-native-shader-execution-tests PRIVATE -UNDEBUG)
      target_compile_options(
        zevryon-native-shader-execution-equivalence-tests PRIVATE -UNDEBUG)
      if(TARGET zevryon-native-shader-execution-metal-tests)
        target_compile_options(
          zevryon-native-shader-execution-metal-tests PRIVATE -UNDEBUG)
        target_compile_options(
          zevryon-native-shader-execution-metal-benchmark PRIVATE -UNDEBUG)
      endif()
      if(TARGET zevryon-native-shader-execution-vulkan-tests)
        target_compile_options(
          zevryon-native-shader-execution-vulkan-tests PRIVATE -UNDEBUG)
        target_compile_options(
          zevryon-native-shader-execution-vulkan-benchmark PRIVATE -UNDEBUG)
      endif()
    endif()

    add_test(
      NAME native-shader-execution-tests
      COMMAND zevryon-native-shader-execution-tests)
    add_test(
      NAME native-shader-execution-equivalence-tests
      COMMAND zevryon-native-shader-execution-equivalence-tests)
    if(TARGET zevryon-native-shader-execution-d3d12-tests)
      add_test(
        NAME native-shader-execution-d3d12-tests
        COMMAND zevryon-native-shader-execution-d3d12-tests)
    endif()
    if(TARGET zevryon-native-shader-execution-metal-tests)
      add_test(
        NAME native-shader-execution-metal-tests
        COMMAND zevryon-native-shader-execution-metal-tests)
    endif()
    if(TARGET zevryon-native-shader-execution-vulkan-tests)
      add_test(
        NAME native-shader-execution-vulkan-tests
        COMMAND zevryon-native-shader-execution-vulkan-tests)
    endif()
  endif()
endif()
