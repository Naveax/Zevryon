cmake_minimum_required(VERSION 3.20)
project(zevryon_rust_unicode_bridge LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ZEVRYON_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(ZEVRYON_RUST_MANIFEST "${ZEVRYON_ROOT}/rust/Cargo.toml")
set(ZEVRYON_RUST_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/cargo-target")

find_program(ZEVRYON_CARGO_EXECUTABLE cargo REQUIRED)

if(WIN32)
  set(ZEVRYON_RUST_UNICODE_FFI_LIBRARY
      "${ZEVRYON_RUST_TARGET_DIR}/release/zevryon_unicode_ffi.lib")
else()
  set(ZEVRYON_RUST_UNICODE_FFI_LIBRARY
      "${ZEVRYON_RUST_TARGET_DIR}/release/libzevryon_unicode_ffi.a")
endif()

file(GLOB_RECURSE ZEVRYON_RUST_UNICODE_SOURCES CONFIGURE_DEPENDS
  "${ZEVRYON_ROOT}/rust/crates/zevryon-unicode-*/*.toml"
  "${ZEVRYON_ROOT}/rust/crates/zevryon-unicode-*/src/*.rs")

add_custom_command(
  OUTPUT "${ZEVRYON_RUST_UNICODE_FFI_LIBRARY}"
  COMMAND
    "${CMAKE_COMMAND}" -E env
    "CARGO_TARGET_DIR=${ZEVRYON_RUST_TARGET_DIR}"
    "${ZEVRYON_CARGO_EXECUTABLE}" build
      --manifest-path "${ZEVRYON_RUST_MANIFEST}"
      --package zevryon-unicode-ffi
      --release
      --locked
  DEPENDS
    ${ZEVRYON_RUST_UNICODE_SOURCES}
    "${ZEVRYON_RUST_MANIFEST}"
    "${ZEVRYON_ROOT}/rust/Cargo.lock"
    "${ZEVRYON_ROOT}/rust/rust-toolchain.toml"
  WORKING_DIRECTORY "${ZEVRYON_ROOT}/rust"
  COMMENT "Building the Zevryon Rust Unicode static library"
  VERBATIM)

add_custom_target(
  zevryon-rust-unicode-ffi-build
  DEPENDS "${ZEVRYON_RUST_UNICODE_FFI_LIBRARY}")

add_library(zevryon-rust-unicode-ffi STATIC IMPORTED GLOBAL)
set_target_properties(
  zevryon-rust-unicode-ffi
  PROPERTIES IMPORTED_LOCATION "${ZEVRYON_RUST_UNICODE_FFI_LIBRARY}")

function(zevryon_strict_options target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX)
  else()
    target_compile_options(
      ${target}
      PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
  endif()
endfunction()

function(zevryon_cpp_options target)
  zevryon_strict_options(${target})
  if(MSVC)
    target_compile_options(${target} PRIVATE /permissive- /EHsc)
  endif()
endfunction()

function(zevryon_link_rust_unicode_runtime target)
  target_link_libraries(${target} PRIVATE zevryon-rust-unicode-ffi)
  add_dependencies(${target} zevryon-rust-unicode-ffi-build)

  if(MSVC)
    target_link_libraries(
      ${target}
      PRIVATE advapi32 bcrypt ntdll userenv ws2_32)
  elseif(APPLE)
    find_library(ZEVRYON_SECURITY_FRAMEWORK Security REQUIRED)
    find_library(ZEVRYON_FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(ZEVRYON_ICONV_LIBRARY iconv REQUIRED)
    target_link_libraries(
      ${target}
      PRIVATE
        "${ZEVRYON_SECURITY_FRAMEWORK}"
        "${ZEVRYON_FOUNDATION_FRAMEWORK}"
        "${ZEVRYON_ICONV_LIBRARY}")
  else()
    find_package(Threads REQUIRED)
    target_link_libraries(${target} PRIVATE Threads::Threads dl m rt util)
  endif()
endfunction()

add_library(zevryon-rust-unicode-bridge STATIC
  "${ZEVRYON_ROOT}/src/unicode_stream.cpp"
  "${ZEVRYON_ROOT}/src/rust_unicode_stream_decoder.cpp")
target_include_directories(
  zevryon-rust-unicode-bridge
  PUBLIC "${ZEVRYON_ROOT}/src")
zevryon_cpp_options(zevryon-rust-unicode-bridge)

add_executable(
  zevryon-rust-unicode-stream-tests
  "${ZEVRYON_ROOT}/tests/rust_unicode_stream_equivalence_tests.cpp")
target_link_libraries(
  zevryon-rust-unicode-stream-tests PRIVATE zevryon-rust-unicode-bridge)
zevryon_link_rust_unicode_runtime(zevryon-rust-unicode-stream-tests)
zevryon_cpp_options(zevryon-rust-unicode-stream-tests)

add_executable(
  zevryon-rust-unicode-c-abi-tests
  "${ZEVRYON_ROOT}/tests/rust_unicode_ffi_c_abi_tests.c")
target_include_directories(
  zevryon-rust-unicode-c-abi-tests PRIVATE "${ZEVRYON_ROOT}/src")
zevryon_link_rust_unicode_runtime(zevryon-rust-unicode-c-abi-tests)
zevryon_strict_options(zevryon-rust-unicode-c-abi-tests)

include(CTest)
add_test(
  NAME rust-unicode-stream-equivalence
  COMMAND zevryon-rust-unicode-stream-tests)
add_test(
  NAME rust-unicode-c-abi-runtime
  COMMAND zevryon-rust-unicode-c-abi-tests)
