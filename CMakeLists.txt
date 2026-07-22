cmake_minimum_required(VERSION 3.20)
project(zevryon_massivedoc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(ZEVRYON_ENABLE_SANITIZERS "Enable ASan and UBSan" OFF)

function(zevryon_options target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX /permissive- /EHsc)
    if(ZEVRYON_ENABLE_SANITIZERS)
      target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
      target_link_options(${target} PRIVATE /INCREMENTAL:NO)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
    if(ZEVRYON_ENABLE_SANITIZERS)
      target_compile_options(${target} PRIVATE -fno-omit-frame-pointer -fsanitize=address,undefined)
      target_link_options(${target} PRIVATE -fno-omit-frame-pointer -fsanitize=address,undefined)
    endif()
  endif()
endfunction()

set(ZEVRYON_MASSIVEDOC_PARTS
  src/massivedoc_store_part00.inc
  src/massivedoc_store_part00_tail.inc
  src/massivedoc_store_part01.inc
  src/massivedoc_store_part02.inc
  src/massivedoc_store_part03.inc
  src/massivedoc_store_part04.inc
  src/massivedoc_store_part05.inc
)
set(ZEVRYON_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(ZEVRYON_MASSIVEDOC_SOURCE "${ZEVRYON_GENERATED_DIR}/massivedoc_store.cpp")
file(MAKE_DIRECTORY "${ZEVRYON_GENERATED_DIR}")
file(WRITE "${ZEVRYON_MASSIVEDOC_SOURCE}" "")
foreach(part IN LISTS ZEVRYON_MASSIVEDOC_PARTS)
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${part}" part_content)
  file(APPEND "${ZEVRYON_MASSIVEDOC_SOURCE}" "${part_content}")
endforeach()

add_library(zevryon-massivedoc-core STATIC
  "${ZEVRYON_MASSIVEDOC_SOURCE}"
  src/compact_document.cpp
  src/layout_window.cpp
)
target_include_directories(zevryon-massivedoc-core PUBLIC src)
zevryon_options(zevryon-massivedoc-core)

add_executable(zevryon-massivedoc src/main.cpp)
target_link_libraries(zevryon-massivedoc PRIVATE zevryon-massivedoc-core)
zevryon_options(zevryon-massivedoc)

include(CTest)
if(BUILD_TESTING)
  add_executable(zevryon-massivedoc-tests tests/massivedoc_store_tests.cpp)
  target_link_libraries(zevryon-massivedoc-tests PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-massivedoc-tests)
  add_test(NAME massivedoc-store-tests COMMAND zevryon-massivedoc-tests)

  add_executable(zevryon-compact-document-tests tests/compact_document_tests.cpp)
  target_link_libraries(zevryon-compact-document-tests PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-compact-document-tests)
  add_test(NAME compact-document-tests COMMAND zevryon-compact-document-tests)

  add_executable(zevryon-layout-window-tests tests/layout_window_tests.cpp)
  target_link_libraries(zevryon-layout-window-tests PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-layout-window-tests)
  add_test(NAME layout-window-tests COMMAND zevryon-layout-window-tests)
endif()
