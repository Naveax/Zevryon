cmake_minimum_required(VERSION 3.20)
project(zevryon_rust_bridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ZEVRYON_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
set(ZEVRYON_RUST_MANIFEST "${ZEVRYON_ROOT}/rust/Cargo.toml")
set(ZEVRYON_RUST_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/cargo-target")

find_program(ZEVRYON_CARGO_EXECUTABLE cargo REQUIRED)

if(WIN32)
  set(ZEVRYON_RUST_FFI_LIBRARY
      "${ZEVRYON_RUST_TARGET_DIR}/release/zevryon_ffi.lib")
else()
  set(ZEVRYON_RUST_FFI_LIBRARY
      "${ZEVRYON_RUST_TARGET_DIR}/release/libzevryon_ffi.a")
endif()

file(GLOB_RECURSE ZEVRYON_RUST_SOURCES CONFIGURE_DEPENDS
  "${ZEVRYON_ROOT}/rust/crates/*.rs"
  "${ZEVRYON_ROOT}/rust/crates/*/Cargo.toml")

add_custom_command(
  OUTPUT "${ZEVRYON_RUST_FFI_LIBRARY}"
  COMMAND
    "${CMAKE_COMMAND}" -E env
    "CARGO_TARGET_DIR=${ZEVRYON_RUST_TARGET_DIR}"
    "${ZEVRYON_CARGO_EXECUTABLE}" build
      --manifest-path "${ZEVRYON_RUST_MANIFEST}"
      --package zevryon-ffi
      --release
      --locked
  DEPENDS
    ${ZEVRYON_RUST_SOURCES}
    "${ZEVRYON_RUST_MANIFEST}"
    "${ZEVRYON_ROOT}/rust/Cargo.lock"
    "${ZEVRYON_ROOT}/rust/rust-toolchain.toml"
  WORKING_DIRECTORY "${ZEVRYON_ROOT}/rust"
  COMMENT "Building the Zevryon Rust core static library"
  VERBATIM)

add_custom_target(
  zevryon-rust-ffi-build
  DEPENDS "${ZEVRYON_RUST_FFI_LIBRARY}")

add_library(zevryon-rust-ffi STATIC IMPORTED GLOBAL)
set_target_properties(
  zevryon-rust-ffi
  PROPERTIES IMPORTED_LOCATION "${ZEVRYON_RUST_FFI_LIBRARY}")

add_library(zevryon-rust-ledger-bridge STATIC
  "${ZEVRYON_ROOT}/src/resource_ledger.cpp"
  "${ZEVRYON_ROOT}/src/rust_resource_ledger.cpp")
target_include_directories(
  zevryon-rust-ledger-bridge
  PUBLIC "${ZEVRYON_ROOT}/src")
add_dependencies(zevryon-rust-ledger-bridge zevryon-rust-ffi-build)

if(MSVC)
  target_compile_options(
    zevryon-rust-ledger-bridge PRIVATE /W4 /WX /permissive- /EHsc)
else()
  target_compile_options(
    zevryon-rust-ledger-bridge
    PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
endif()

add_executable(
  zevryon-rust-resource-ledger-tests
  "${ZEVRYON_ROOT}/tests/rust_resource_ledger_equivalence_tests.cpp")
target_link_libraries(
  zevryon-rust-resource-ledger-tests
  PRIVATE zevryon-rust-ledger-bridge zevryon-rust-ffi)
add_dependencies(zevryon-rust-resource-ledger-tests zevryon-rust-ffi-build)

if(MSVC)
  target_compile_options(
    zevryon-rust-resource-ledger-tests PRIVATE /W4 /WX /permissive- /EHsc)
  target_link_libraries(
    zevryon-rust-resource-ledger-tests
    PRIVATE advapi32 bcrypt ntdll userenv ws2_32)
elseif(APPLE)
  target_compile_options(
    zevryon-rust-resource-ledger-tests
    PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
  find_library(ZEVRYON_SECURITY_FRAMEWORK Security REQUIRED)
  find_library(ZEVRYON_FOUNDATION_FRAMEWORK Foundation REQUIRED)
  find_library(ZEVRYON_ICONV_LIBRARY iconv REQUIRED)
  target_link_libraries(
    zevryon-rust-resource-ledger-tests
    PRIVATE
      "${ZEVRYON_SECURITY_FRAMEWORK}"
      "${ZEVRYON_FOUNDATION_FRAMEWORK}"
      "${ZEVRYON_ICONV_LIBRARY}")
else()
  target_compile_options(
    zevryon-rust-resource-ledger-tests
    PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
  find_package(Threads REQUIRED)
  target_link_libraries(
    zevryon-rust-resource-ledger-tests
    PRIVATE Threads::Threads dl m rt util)
endif()

include(CTest)
add_test(
  NAME rust-resource-ledger-equivalence
  COMMAND zevryon-rust-resource-ledger-tests)
