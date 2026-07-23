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
  src/bidi_explicit.cpp
  src/bidi_implicit.cpp
  src/bidi_neutral.cpp
  src/bidi_sequence.cpp
  src/bidi_visual.cpp
  src/bidi_weak.cpp
  src/compact_document.cpp
  src/grapheme_segmenter.cpp
  src/layout_checkpoint.cpp
  src/layout_window.cpp
  src/ledger_memory_resource.cpp
  src/resource_ledger.cpp
  src/script_run.cpp
  src/unicode_bidi.cpp
  src/unicode_bidi_brackets.cpp
  src/unicode_grapheme_data.cpp
  src/unicode_script.cpp
  src/unicode_stream.cpp
  src/zenith_layout_window.cpp
  src/zenith_hot_scroll.cpp
)
target_include_directories(zevryon-massivedoc-core PUBLIC src)
zevryon_options(zevryon-massivedoc-core)

add_executable(zevryon-massivedoc src/main.cpp)
target_link_libraries(zevryon-massivedoc PRIVATE zevryon-massivedoc-core)
zevryon_options(zevryon-massivedoc)

add_executable(zevryon-zenith-layout src/zenith_main.cpp)
target_link_libraries(zevryon-zenith-layout PRIVATE zevryon-massivedoc-core)
zevryon_options(zevryon-zenith-layout)

add_executable(zevryon-zenith-hot src/zenith_hot_main.cpp)
target_link_libraries(zevryon-zenith-hot PRIVATE zevryon-massivedoc-core)
zevryon_options(zevryon-zenith-hot)

add_executable(zevryon-unicode-benchmark src/unicode_benchmark_main.cpp)
target_link_libraries(zevryon-unicode-benchmark PRIVATE zevryon-massivedoc-core)
zevryon_options(zevryon-unicode-benchmark)

add_executable(zevryon-grapheme-conformance src/grapheme_conformance_main.cpp)
target_link_libraries(zevryon-grapheme-conformance PRIVATE zevryon-massivedoc-core)
zevryon_options(zevryon-grapheme-conformance)

add_executable(zevryon-grapheme-benchmark src/grapheme_benchmark_main.cpp)
target_link_libraries(zevryon-grapheme-benchmark PRIVATE zevryon-massivedoc-core)
zevryon_options(zevryon-grapheme-benchmark)

add_executable(zevryon-script-conformance src/script_conformance_main.cpp)
target_link_libraries(zevryon-script-conformance PRIVATE zevryon-massivedoc-core)
zevryon_options(zevryon-script-conformance)

add_executable(zevryon-script-run-benchmark src/script_run_benchmark_main.cpp)
target_link_libraries(zevryon-script-run-benchmark PRIVATE zevryon-massivedoc-core)
zevryon_options(zevryon-script-run-benchmark)

add_executable(zevryon-bidi-conformance src/bidi_conformance_main.cpp)
target_link_libraries(zevryon-bidi-conformance PRIVATE zevryon-massivedoc-core)
zevryon_options(zevryon-bidi-conformance)

add_executable(zevryon-bidi-benchmark src/bidi_benchmark_main.cpp)
target_link_libraries(zevryon-bidi-benchmark PRIVATE zevryon-massivedoc-core)
zevryon_options(zevryon-bidi-benchmark)

add_executable(zevryon-bidi-sequence-conformance src/bidi_sequence_conformance_main.cpp)
target_link_libraries(zevryon-bidi-sequence-conformance PRIVATE zevryon-massivedoc-core)
zevryon_options(zevryon-bidi-sequence-conformance)

add_executable(zevryon-bidi-sequence-benchmark src/bidi_sequence_benchmark_main.cpp)
target_link_libraries(zevryon-bidi-sequence-benchmark PRIVATE zevryon-massivedoc-core)
zevryon_options(zevryon-bidi-sequence-benchmark)

add_executable(zevryon-bidi-weak-conformance src/bidi_weak_conformance_main.cpp)
target_link_libraries(zevryon-bidi-weak-conformance PRIVATE zevryon-massivedoc-core)
zevryon_options(zevryon-bidi-weak-conformance)

add_executable(zevryon-bidi-weak-benchmark src/bidi_weak_benchmark_main.cpp)
target_link_libraries(zevryon-bidi-weak-benchmark PRIVATE zevryon-massivedoc-core)
zevryon_options(zevryon-bidi-weak-benchmark)

add_executable(zevryon-bidi-bracket-conformance src/bidi_bracket_conformance_main.cpp)
target_link_libraries(zevryon-bidi-bracket-conformance PRIVATE zevryon-massivedoc-core)
zevryon_options(zevryon-bidi-bracket-conformance)

add_executable(zevryon-bidi-neutral-benchmark src/bidi_neutral_benchmark_main.cpp)
target_link_libraries(zevryon-bidi-neutral-benchmark PRIVATE zevryon-massivedoc-core)
zevryon_options(zevryon-bidi-neutral-benchmark)

add_executable(zevryon-bidi-implicit-conformance src/bidi_implicit_conformance_main.cpp)
target_link_libraries(zevryon-bidi-implicit-conformance PRIVATE zevryon-massivedoc-core)
zevryon_options(zevryon-bidi-implicit-conformance)

add_executable(zevryon-bidi-implicit-benchmark src/bidi_implicit_benchmark_main.cpp)
target_link_libraries(zevryon-bidi-implicit-benchmark PRIVATE zevryon-massivedoc-core)
zevryon_options(zevryon-bidi-implicit-benchmark)

add_executable(zevryon-bidi-visual-conformance src/bidi_visual_conformance_main.cpp)
target_link_libraries(zevryon-bidi-visual-conformance PRIVATE zevryon-massivedoc-core)
zevryon_options(zevryon-bidi-visual-conformance)

add_executable(zevryon-bidi-visual-benchmark src/bidi_visual_benchmark_main.cpp)
target_link_libraries(zevryon-bidi-visual-benchmark PRIVATE zevryon-massivedoc-core)
zevryon_options(zevryon-bidi-visual-benchmark)

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

  add_executable(zevryon-layout-checkpoint-tests tests/layout_checkpoint_tests.cpp)
  target_link_libraries(zevryon-layout-checkpoint-tests PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-layout-checkpoint-tests)
  add_test(NAME layout-checkpoint-tests COMMAND zevryon-layout-checkpoint-tests)

  add_executable(zevryon-zenith-layout-tests tests/zenith_layout_window_tests.cpp)
  target_link_libraries(zevryon-zenith-layout-tests PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-zenith-layout-tests)
  add_test(NAME zenith-layout-tests COMMAND zevryon-zenith-layout-tests)

  add_executable(zevryon-zenith-hot-tests tests/zenith_hot_scroll_tests.cpp)
  target_link_libraries(zevryon-zenith-hot-tests PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-zenith-hot-tests)
  add_test(NAME zenith-hot-scroll-tests COMMAND zevryon-zenith-hot-tests)

  add_executable(zevryon-resource-ledger-tests tests/resource_ledger_tests.cpp)
  target_link_libraries(zevryon-resource-ledger-tests PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-resource-ledger-tests)
  add_test(NAME resource-ledger-tests COMMAND zevryon-resource-ledger-tests)

  add_executable(zevryon-unicode-stream-tests tests/unicode_stream_tests.cpp)
  target_link_libraries(zevryon-unicode-stream-tests PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-unicode-stream-tests)
  add_test(NAME unicode-stream-tests COMMAND zevryon-unicode-stream-tests)

  add_executable(zevryon-grapheme-segmenter-tests tests/grapheme_segmenter_tests.cpp)
  target_link_libraries(zevryon-grapheme-segmenter-tests PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-grapheme-segmenter-tests)
  add_test(NAME grapheme-segmenter-tests COMMAND zevryon-grapheme-segmenter-tests)

  add_executable(zevryon-script-run-tests tests/script_run_tests.cpp)
  target_link_libraries(zevryon-script-run-tests PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-script-run-tests)
  add_test(NAME script-run-tests COMMAND zevryon-script-run-tests)

  add_executable(zevryon-bidi-explicit-tests tests/bidi_explicit_tests.cpp)
  target_link_libraries(zevryon-bidi-explicit-tests PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-bidi-explicit-tests)
  add_test(NAME bidi-explicit-tests COMMAND zevryon-bidi-explicit-tests)

  add_executable(zevryon-bidi-sequence-tests tests/bidi_sequence_tests.cpp)
  target_link_libraries(zevryon-bidi-sequence-tests PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-bidi-sequence-tests)
  add_test(NAME bidi-sequence-tests COMMAND zevryon-bidi-sequence-tests)

  add_executable(zevryon-bidi-weak-tests tests/bidi_weak_tests.cpp)
  target_link_libraries(zevryon-bidi-weak-tests PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-bidi-weak-tests)
  add_test(NAME bidi-weak-tests COMMAND zevryon-bidi-weak-tests)

  add_executable(zevryon-bidi-neutral-tests tests/bidi_neutral_tests.cpp)
  target_link_libraries(zevryon-bidi-neutral-tests PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-bidi-neutral-tests)
  add_test(NAME bidi-neutral-tests COMMAND zevryon-bidi-neutral-tests)

  add_executable(zevryon-bidi-implicit-tests tests/bidi_implicit_tests.cpp)
  target_link_libraries(zevryon-bidi-implicit-tests PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-bidi-implicit-tests)
  add_test(NAME bidi-implicit-tests COMMAND zevryon-bidi-implicit-tests)

  add_executable(zevryon-bidi-visual-tests tests/bidi_visual_tests.cpp)
  target_link_libraries(zevryon-bidi-visual-tests PRIVATE zevryon-massivedoc-core)
  zevryon_options(zevryon-bidi-visual-tests)
  add_test(NAME bidi-visual-tests COMMAND zevryon-bidi-visual-tests)
endif()
