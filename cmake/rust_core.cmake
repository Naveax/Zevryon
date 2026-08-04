option(
  ZEVRYON_RUST_LEDGER_AUTHORITATIVE
  "Use Rust as the production ResourceLedger authority while retaining the C++ verifier"
  OFF)
option(
  ZEVRYON_RUST_UNICODE_SHADOW
  "Mirror production Utf8StreamDecoder operations into Rust"
  OFF)
option(
  ZEVRYON_RUST_UNICODE_SHADOW_STRICT
  "Abort immediately on a production Rust Unicode shadow mismatch"
  OFF)

# Authority promotion and Unicode shadow execution are deliberately opt-in.
# Either feature selects the shared Rust toolchain without changing the normal
# cache values used by default C++ builds.
if(ZEVRYON_RUST_LEDGER_AUTHORITATIVE)
  set(ZEVRYON_ENABLE_RUST_CORE ON)
  set(ZEVRYON_RUST_LEDGER_SHADOW ON)
endif()
if(ZEVRYON_RUST_UNICODE_SHADOW)
  set(ZEVRYON_ENABLE_RUST_CORE ON)
endif()
if(ZEVRYON_RUST_UNICODE_SHADOW_STRICT AND
   NOT ZEVRYON_RUST_UNICODE_SHADOW)
  message(FATAL_ERROR
    "ZEVRYON_RUST_UNICODE_SHADOW_STRICT requires ZEVRYON_RUST_UNICODE_SHADOW=ON")
endif()
if(ZEVRYON_RUST_UNICODE_SHADOW_STRICT)
  set(ZEVRYON_RUST_UNICODE_SHADOW_STRICT_VALUE 1)
else()
  set(ZEVRYON_RUST_UNICODE_SHADOW_STRICT_VALUE 0)
endif()

if(NOT ZEVRYON_ENABLE_RUST_CORE)
  return()
endif()

set(ZEVRYON_RUST_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.toml")
set(ZEVRYON_RUST_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/rust-target")
find_program(ZEVRYON_CARGO_EXECUTABLE cargo REQUIRED)

if(WIN32)
  set(ZEVRYON_RUST_FFI_LIBRARY
      "${ZEVRYON_RUST_TARGET_DIR}/release/zevryon_ffi.lib")
  set(ZEVRYON_RUST_UNICODE_FFI_LIBRARY
      "${ZEVRYON_RUST_TARGET_DIR}/release/zevryon_unicode_ffi.lib")
else()
  set(ZEVRYON_RUST_FFI_LIBRARY
      "${ZEVRYON_RUST_TARGET_DIR}/release/libzevryon_ffi.a")
  set(ZEVRYON_RUST_UNICODE_FFI_LIBRARY
      "${ZEVRYON_RUST_TARGET_DIR}/release/libzevryon_unicode_ffi.a")
endif()

file(GLOB_RECURSE ZEVRYON_RUST_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/rust/crates/*.rs"
  "${CMAKE_CURRENT_SOURCE_DIR}/rust/crates/*/Cargo.toml")

# Strict certification deliberately terminates at the first mismatch. MSVC can
# consequently diagnose the immediately following control-flow joins as C4702.
# Scope the suppression only to the selected strict translation units.
if(MSVC AND ZEVRYON_RUST_LEDGER_SHADOW_STRICT)
  set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/resource_ledger.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/resource_ledger_authoritative.cpp"
    PROPERTIES COMPILE_OPTIONS "/wd4702")
endif()
if(MSVC AND ZEVRYON_RUST_UNICODE_SHADOW_STRICT)
  set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/unicode_stream.cpp"
    PROPERTIES COMPILE_OPTIONS "/wd4702")
endif()

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
    "${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.lock"
    "${CMAKE_CURRENT_SOURCE_DIR}/rust/rust-toolchain.toml"
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/rust"
  COMMENT "Building the Zevryon Rust core static library"
  VERBATIM)

add_custom_target(
  zevryon-rust-ffi-build
  DEPENDS "${ZEVRYON_RUST_FFI_LIBRARY}")

add_library(zevryon-rust-ffi STATIC IMPORTED GLOBAL)
set_target_properties(
  zevryon-rust-ffi
  PROPERTIES IMPORTED_LOCATION "${ZEVRYON_RUST_FFI_LIBRARY}")

if(ZEVRYON_RUST_UNICODE_SHADOW)
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
      ${ZEVRYON_RUST_SOURCES}
      "${ZEVRYON_RUST_MANIFEST}"
      "${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.lock"
      "${CMAKE_CURRENT_SOURCE_DIR}/rust/rust-toolchain.toml"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/rust"
    COMMENT "Building the Zevryon Rust Unicode static library"
    VERBATIM)

  add_custom_target(
    zevryon-rust-unicode-ffi-build
    DEPENDS "${ZEVRYON_RUST_UNICODE_FFI_LIBRARY}")

  add_library(zevryon-rust-unicode-ffi STATIC IMPORTED GLOBAL)
  set_target_properties(
    zevryon-rust-unicode-ffi
    PROPERTIES IMPORTED_LOCATION "${ZEVRYON_RUST_UNICODE_FFI_LIBRARY}")

  add_library(zevryon-rust-unicode-runtime INTERFACE)
  target_link_libraries(
    zevryon-rust-unicode-runtime
    INTERFACE zevryon-rust-unicode-ffi)
  add_dependencies(
    zevryon-rust-unicode-runtime
    zevryon-rust-unicode-ffi-build)

  if(MSVC)
    target_link_libraries(
      zevryon-rust-unicode-runtime
      INTERFACE advapi32 bcrypt ntdll userenv ws2_32)
  elseif(APPLE)
    find_library(ZEVRYON_UNICODE_SECURITY_FRAMEWORK Security REQUIRED)
    find_library(ZEVRYON_UNICODE_FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(ZEVRYON_UNICODE_ICONV_LIBRARY iconv REQUIRED)
    target_link_libraries(
      zevryon-rust-unicode-runtime
      INTERFACE
        "${ZEVRYON_UNICODE_SECURITY_FRAMEWORK}"
        "${ZEVRYON_UNICODE_FOUNDATION_FRAMEWORK}"
        "${ZEVRYON_UNICODE_ICONV_LIBRARY}")
  else()
    find_package(Threads REQUIRED)
    target_link_libraries(
      zevryon-rust-unicode-runtime
      INTERFACE Threads::Threads dl m rt util)
  endif()

  add_compile_definitions(
    ZEVRYON_UTF8_RUST_SHADOW=1
    ZEVRYON_RUST_UNICODE_SHADOW_STRICT=${ZEVRYON_RUST_UNICODE_SHADOW_STRICT_VALUE})
  link_libraries(zevryon-rust-unicode-runtime)
endif()

function(zevryon_link_rust_core target)
  if(ZEVRYON_RUST_LEDGER_AUTHORITATIVE)
    # The production target was declared with resource_ledger.cpp in the root
    # source list. Mark it non-compiling and substitute the authoritative
    # implementation only for this opt-in configuration.
    set_source_files_properties(
      "${CMAKE_CURRENT_SOURCE_DIR}/src/resource_ledger.cpp"
      PROPERTIES HEADER_FILE_ONLY TRUE)
    target_sources(
      ${target}
      PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/resource_ledger_authoritative.cpp")
    target_compile_definitions(
      ${target}
      PUBLIC ZEVRYON_RESOURCE_LEDGER_RUST_AUTHORITATIVE=1
      PRIVATE ZEVRYON_RESOURCE_LEDGER_AUTHORITY_TEST_HOOKS=1)
  endif()

  target_link_libraries(${target} PUBLIC zevryon-rust-ffi)
  add_dependencies(${target} zevryon-rust-ffi-build)

  if(MSVC)
    target_link_libraries(
      ${target}
      PUBLIC advapi32 bcrypt ntdll userenv ws2_32)
  elseif(APPLE)
    find_library(ZEVRYON_SECURITY_FRAMEWORK Security REQUIRED)
    find_library(ZEVRYON_FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(ZEVRYON_ICONV_LIBRARY iconv REQUIRED)
    target_link_libraries(
      ${target}
      PUBLIC
        "${ZEVRYON_SECURITY_FRAMEWORK}"
        "${ZEVRYON_FOUNDATION_FRAMEWORK}"
        "${ZEVRYON_ICONV_LIBRARY}")
  else()
    find_package(Threads REQUIRED)
    target_link_libraries(
      ${target}
      PUBLIC Threads::Threads dl m rt util)
  endif()

  if(ZEVRYON_RUST_LEDGER_AUTHORITATIVE AND
     NOT TARGET zevryon-rust-authority-tests)
    include(CTest)
    add_executable(
      zevryon-rust-authority-tests
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/rust_resource_ledger_authority_tests.cpp")
    target_compile_definitions(
      zevryon-rust-authority-tests
      PRIVATE ZEVRYON_RESOURCE_LEDGER_AUTHORITY_TEST_HOOKS=1)
    target_link_libraries(
      zevryon-rust-authority-tests PRIVATE ${target})
    zevryon_options(zevryon-rust-authority-tests)

    if(BUILD_TESTING)
      add_test(
        NAME rust-resource-ledger-authority
        COMMAND zevryon-rust-authority-tests --positive)
      if(NOT ZEVRYON_RUST_LEDGER_SHADOW_STRICT)
        add_test(
          NAME rust-resource-ledger-authority-fault
          COMMAND zevryon-rust-authority-tests --fault)
      endif()
    endif()
  endif()
endfunction()
