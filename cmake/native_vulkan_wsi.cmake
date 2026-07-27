if(TARGET zevryon-native-gpu-sdk-execution AND
   TARGET zevryon-native-window-swapchain)
  target_sources(
    zevryon-native-gpu-sdk-execution
    PRIVATE src/native_vulkan_wsi_stub.cpp)

  set(ZEVRYON_VULKAN_WSI_AVAILABLE OFF)
  set(ZEVRYON_VULKAN_WSI_DEFINITIONS ZEVRYON_HAS_VULKAN_WSI=1)
  set(ZEVRYON_VULKAN_WSI_LINK_LIBRARIES Vulkan::Vulkan)

  find_package(Vulkan QUIET)
  if(Vulkan_FOUND)
    if(WIN32)
      set(ZEVRYON_VULKAN_WSI_AVAILABLE ON)
      list(APPEND
        ZEVRYON_VULKAN_WSI_DEFINITIONS
        ZEVRYON_VULKAN_WSI_HAS_WIN32=1)
    elseif(UNIX AND NOT APPLE)
      find_package(PkgConfig QUIET)
      if(PkgConfig_FOUND)
        pkg_check_modules(ZEVRYON_XCB QUIET IMPORTED_TARGET xcb)
        pkg_check_modules(
          ZEVRYON_WAYLAND QUIET IMPORTED_TARGET wayland-client)
        if(ZEVRYON_XCB_FOUND)
          set(ZEVRYON_VULKAN_WSI_AVAILABLE ON)
          list(APPEND
            ZEVRYON_VULKAN_WSI_DEFINITIONS
            ZEVRYON_VULKAN_WSI_HAS_XCB=1)
          list(APPEND
            ZEVRYON_VULKAN_WSI_LINK_LIBRARIES
            PkgConfig::ZEVRYON_XCB)
        endif()
        if(ZEVRYON_WAYLAND_FOUND)
          set(ZEVRYON_VULKAN_WSI_AVAILABLE ON)
          list(APPEND
            ZEVRYON_VULKAN_WSI_DEFINITIONS
            ZEVRYON_VULKAN_WSI_HAS_WAYLAND=1)
          list(APPEND
            ZEVRYON_VULKAN_WSI_LINK_LIBRARIES
            PkgConfig::ZEVRYON_WAYLAND)
        endif()
      endif()
    endif()
  endif()

  if(ZEVRYON_VULKAN_WSI_AVAILABLE)
    target_sources(
      zevryon-native-gpu-sdk-execution
      PRIVATE src/native_gpu_sdk_execution_vulkan_wsi.cpp)
    target_sources(
      zevryon-native-window-swapchain
      PRIVATE src/native_window_swapchain_vulkan.cpp)
    target_compile_definitions(
      zevryon-native-gpu-sdk-execution
      PRIVATE ${ZEVRYON_VULKAN_WSI_DEFINITIONS})
    target_compile_definitions(
      zevryon-native-window-swapchain
      PRIVATE ${ZEVRYON_VULKAN_WSI_DEFINITIONS})
    target_link_libraries(
      zevryon-native-gpu-sdk-execution
      PRIVATE ${ZEVRYON_VULKAN_WSI_LINK_LIBRARIES})
    target_link_libraries(
      zevryon-native-window-swapchain
      PRIVATE ${ZEVRYON_VULKAN_WSI_LINK_LIBRARIES})

    if(BUILD_TESTING)
      set(ZEVRYON_VULKAN_WSI_TEST_WINDOW_SOURCES
          tests/native_vulkan_wsi_test_window.cpp)
      set(ZEVRYON_VULKAN_WSI_TEST_WINDOW_DEFINITIONS)
      set(ZEVRYON_VULKAN_WSI_TEST_WINDOW_LIBRARIES)

      if(WIN32)
        list(APPEND
          ZEVRYON_VULKAN_WSI_TEST_WINDOW_LIBRARIES user32)
      elseif(UNIX AND NOT APPLE)
        if(ZEVRYON_XCB_FOUND)
          list(APPEND
            ZEVRYON_VULKAN_WSI_TEST_WINDOW_DEFINITIONS
            ZEVRYON_VULKAN_WSI_TEST_XCB=1)
          list(APPEND
            ZEVRYON_VULKAN_WSI_TEST_WINDOW_LIBRARIES
            PkgConfig::ZEVRYON_XCB)
        endif()
        if(ZEVRYON_WAYLAND_FOUND)
          pkg_check_modules(
            ZEVRYON_WAYLAND_PROTOCOLS QUIET wayland-protocols)
          find_program(ZEVRYON_WAYLAND_SCANNER wayland-scanner)
          if(ZEVRYON_WAYLAND_PROTOCOLS_FOUND AND
             ZEVRYON_WAYLAND_SCANNER)
            pkg_get_variable(
              ZEVRYON_WAYLAND_PROTOCOLS_DIR
              wayland-protocols pkgdatadir)
            set(
              ZEVRYON_XDG_SHELL_XML
              "${ZEVRYON_WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml")
            set(
              ZEVRYON_XDG_SHELL_HEADER
              "${CMAKE_CURRENT_BINARY_DIR}/generated/xdg-shell-client-protocol.h")
            set(
              ZEVRYON_XDG_SHELL_CODE
              "${CMAKE_CURRENT_BINARY_DIR}/generated/xdg-shell-protocol.c")
            add_custom_command(
              OUTPUT
                "${ZEVRYON_XDG_SHELL_HEADER}"
                "${ZEVRYON_XDG_SHELL_CODE}"
              COMMAND
                ${CMAKE_COMMAND} -E make_directory
                "${CMAKE_CURRENT_BINARY_DIR}/generated"
              COMMAND
                "${ZEVRYON_WAYLAND_SCANNER}" client-header
                "${ZEVRYON_XDG_SHELL_XML}"
                "${ZEVRYON_XDG_SHELL_HEADER}"
              COMMAND
                "${ZEVRYON_WAYLAND_SCANNER}" private-code
                "${ZEVRYON_XDG_SHELL_XML}"
                "${ZEVRYON_XDG_SHELL_CODE}"
              DEPENDS "${ZEVRYON_XDG_SHELL_XML}"
              VERBATIM)
            enable_language(C)
            add_library(
              zevryon-xdg-shell-protocol STATIC
              "${ZEVRYON_XDG_SHELL_CODE}")
            target_include_directories(
              zevryon-xdg-shell-protocol
              PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/generated")
            target_link_libraries(
              zevryon-xdg-shell-protocol
              PUBLIC PkgConfig::ZEVRYON_WAYLAND)
            list(APPEND
              ZEVRYON_VULKAN_WSI_TEST_WINDOW_DEFINITIONS
              ZEVRYON_VULKAN_WSI_TEST_WAYLAND=1)
            list(APPEND
              ZEVRYON_VULKAN_WSI_TEST_WINDOW_LIBRARIES
              PkgConfig::ZEVRYON_WAYLAND
              zevryon-xdg-shell-protocol)
          endif()
        endif()
      endif()

      add_library(
        zevryon-vulkan-wsi-test-window STATIC
        ${ZEVRYON_VULKAN_WSI_TEST_WINDOW_SOURCES})
      target_include_directories(
        zevryon-vulkan-wsi-test-window
        PUBLIC tests
        PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
      if(ZEVRYON_VULKAN_WSI_TEST_WINDOW_DEFINITIONS)
        target_compile_definitions(
          zevryon-vulkan-wsi-test-window
          PRIVATE ${ZEVRYON_VULKAN_WSI_TEST_WINDOW_DEFINITIONS})
      endif()
      target_link_libraries(
        zevryon-vulkan-wsi-test-window
        PUBLIC zevryon-native-window-swapchain
        PRIVATE ${ZEVRYON_VULKAN_WSI_TEST_WINDOW_LIBRARIES})
      zevryon_options(zevryon-vulkan-wsi-test-window)

      add_executable(
        zevryon-native-window-swapchain-vulkan-tests
        tests/native_window_swapchain_vulkan_tests.cpp)
      target_link_libraries(
        zevryon-native-window-swapchain-vulkan-tests
        PRIVATE zevryon-vulkan-wsi-test-window)
      zevryon_options(zevryon-native-window-swapchain-vulkan-tests)

      add_executable(
        zevryon-native-window-swapchain-vulkan-benchmark
        src/native_window_swapchain_vulkan_benchmark_main.cpp)
      target_link_libraries(
        zevryon-native-window-swapchain-vulkan-benchmark
        PRIVATE zevryon-vulkan-wsi-test-window)
      zevryon_options(zevryon-native-window-swapchain-vulkan-benchmark)

      if(MSVC)
        target_compile_options(
          zevryon-native-window-swapchain-vulkan-tests PRIVATE /UNDEBUG)
        target_compile_options(
          zevryon-native-window-swapchain-vulkan-benchmark PRIVATE /UNDEBUG)
      else()
        target_compile_options(
          zevryon-native-window-swapchain-vulkan-tests PRIVATE -UNDEBUG)
        target_compile_options(
          zevryon-native-window-swapchain-vulkan-benchmark PRIVATE -UNDEBUG)
      endif()
    endif()
  endif()
endif()
