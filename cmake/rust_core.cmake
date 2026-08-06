option(
  ZEVRYON_RUST_LEDGER_AUTHORITATIVE
  "Use Rust as the production ResourceLedger authority while retaining the C++ verifier"
  OFF)
option(
  ZEVRYON_RUST_MASSIVEDOC_CODEC_AUTHORITATIVE
  "Use Rust as the production MassiveDoc descriptor authority while retaining the C++ reverse shadow"
  OFF)
option(
  ZEVRYON_RUST_UNICODE_SHADOW
  "Mirror production Utf8StreamDecoder operations into Rust"
  OFF)
option(
  ZEVRYON_RUST_UNICODE_SHADOW_STRICT
  "Abort immediately on a production Rust Unicode shadow mismatch"
  OFF)
option(
  ZEVRYON_RUST_UNICODE_SHADOW_TEST_HOOKS
  "Compile diagnostic-only Unicode shadow fault injection hooks"
  OFF)
option(
  ZEVRYON_RUST_UNICODE_AUTHORITATIVE
  "Use Rust as the production UTF-8 decoder authority with C++ reverse shadow"
  OFF)
option(
  ZEVRYON_RUST_UNICODE_AUTHORITY_TEST_HOOKS
  "Compile diagnostic-only C++ reverse-shadow fault injection hooks"
  OFF)

# Authority promotion and shadow execution are deliberately opt-in. Enabling
# any feature selects the shared Rust toolchain without changing default cache
# values used by normal C++ builds.
if(ZEVRYON_RUST_LEDGER_AUTHORITATIVE)
  set(ZEVRYON_ENABLE_RUST_CORE ON)
  set(ZEVRYON_RUST_LEDGER_SHADOW ON)
endif()
if(ZEVRYON_RUST_MASSIVEDOC_CODEC_AUTHORITATIVE)
  set(ZEVRYON_ENABLE_RUST_CORE ON)
  set(ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW ON)
endif()
if(ZEVRYON_RUST_UNICODE_AUTHORITATIVE)
  set(ZEVRYON_ENABLE_RUST_CORE ON)
  set(ZEVRYON_RUST_UNICODE_SHADOW ON)
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
if(ZEVRYON_RUST_UNICODE_SHADOW_TEST_HOOKS AND
   NOT ZEVRYON_RUST_UNICODE_SHADOW)
  message(FATAL_ERROR
    "ZEVRYON_RUST_UNICODE_SHADOW_TEST_HOOKS requires ZEVRYON_RUST_UNICODE_SHADOW=ON")
endif()
if(ZEVRYON_RUST_UNICODE_SHADOW_TEST_HOOKS AND
   ZEVRYON_RUST_UNICODE_SHADOW_STRICT)
  message(FATAL_ERROR
    "Unicode shadow test hooks require diagnostic non-strict mode")
endif()
if(ZEVRYON_RUST_UNICODE_AUTHORITY_TEST_HOOKS AND
   NOT ZEVRYON_RUST_UNICODE_AUTHORITATIVE)
  message(FATAL_ERROR
    "ZEVRYON_RUST_UNICODE_AUTHORITY_TEST_HOOKS requires ZEVRYON_RUST_UNICODE_AUTHORITATIVE=ON")
endif()
if(ZEVRYON_RUST_UNICODE_AUTHORITY_TEST_HOOKS AND
   ZEVRYON_RUST_UNICODE_SHADOW_STRICT)
  message(FATAL_ERROR
    "Unicode authority test hooks require diagnostic non-strict mode")
endif()
if(ZEVRYON_RUST_UNICODE_SHADOW_TEST_HOOKS AND
   ZEVRYON_RUST_UNICODE_AUTHORITATIVE)
  message(FATAL_ERROR
    "Legacy Rust-shadow fault hooks cannot be enabled in Rust authority mode")
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
# Scope the suppression only to selected strict translation units.
if(MSVC AND ZEVRYON_RUST_LEDGER_SHADOW_STRICT)
  set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/resource_ledger.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/resource_ledger_authoritative.cpp"
    PROPERTIES COMPILE_OPTIONS "/wd4702")
endif()
if(MSVC AND ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW_STRICT)
  set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/massivedoc_descriptor_shadow.cpp"
    PROPERTIES COMPILE_OPTIONS "/wd4702")
endif()
if(MSVC AND ZEVRYON_RUST_UNICODE_SHADOW_STRICT)
  set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/unicode_stream.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/unicode_stream_authoritative.cpp"
    PROPERTIES COMPILE_OPTIONS "/wd4702")
endif()
# The diagnostic fault selector intentionally reads a process-local test
# environment variable. MSVC deprecates getenv even for this bounded,
# read-only hook, so suppress C4996 only for the diagnostic translation unit.
if(MSVC AND ZEVRYON_RUST_UNICODE_AUTHORITY_TEST_HOOKS)
  set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/unicode_stream_authoritative.cpp"
    PROPERTIES COMPILE_OPTIONS "/wd4996")
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
  if(ZEVRYON_RUST_UNICODE_SHADOW_TEST_HOOKS)
    add_compile_definitions(ZEVRYON_UTF8_RUST_SHADOW_TEST_HOOKS=1)
  endif()
  link_libraries(zevryon-rust-unicode-runtime)
endif()

if(WIN32)
  set(ZEVRYON_RUST_MASSIVEDOC_FFI_LIBRARY
      "${ZEVRYON_RUST_TARGET_DIR}/release/zevryon_massivedoc_ffi.lib")
else()
  set(ZEVRYON_RUST_MASSIVEDOC_FFI_LIBRARY
      "${ZEVRYON_RUST_TARGET_DIR}/release/libzevryon_massivedoc_ffi.a")
endif()

add_custom_command(
  OUTPUT "${ZEVRYON_RUST_MASSIVEDOC_FFI_LIBRARY}"
  COMMAND
    "${CMAKE_COMMAND}" -E env
    "CARGO_TARGET_DIR=${ZEVRYON_RUST_TARGET_DIR}"
    "${ZEVRYON_CARGO_EXECUTABLE}" build
      --manifest-path "${ZEVRYON_RUST_MANIFEST}"
      --package zevryon-massivedoc-ffi
      --release
      --locked
  DEPENDS
    ${ZEVRYON_RUST_SOURCES}
    "${ZEVRYON_RUST_MANIFEST}"
    "${CMAKE_CURRENT_SOURCE_DIR}/rust/Cargo.lock"
    "${CMAKE_CURRENT_SOURCE_DIR}/rust/rust-toolchain.toml"
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/rust"
  COMMENT "Building the Zevryon Rust MassiveDoc descriptor library"
  VERBATIM)

add_custom_target(
  zevryon-rust-massivedoc-ffi-build
  DEPENDS "${ZEVRYON_RUST_MASSIVEDOC_FFI_LIBRARY}")
add_dependencies(zevryon-rust-massivedoc-ffi-build zevryon-rust-ffi-build)

add_library(zevryon-rust-massivedoc-ffi STATIC IMPORTED GLOBAL)
set_target_properties(
  zevryon-rust-massivedoc-ffi
  PROPERTIES IMPORTED_LOCATION "${ZEVRYON_RUST_MASSIVEDOC_FFI_LIBRARY}")

function(zevryon_link_rust_core target)
  if(ZEVRYON_RUST_LEDGER_AUTHORITATIVE)
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

function(zevryon_link_rust_massivedoc_codec target)
  if(ZEVRYON_RUST_MASSIVEDOC_CODEC_AUTHORITATIVE)
    target_compile_definitions(
      ${target}
      PUBLIC ZEVRYON_RUST_MASSIVEDOC_CODEC_AUTHORITATIVE=1
      PRIVATE ZEVRYON_MASSIVEDOC_CODEC_AUTHORITY_TEST_HOOKS=1)
  endif()

  target_link_libraries(${target} PUBLIC zevryon-rust-massivedoc-ffi)
  add_dependencies(${target} zevryon-rust-massivedoc-ffi-build)

  if(MSVC)
    target_link_libraries(
      ${target}
      PUBLIC advapi32 bcrypt ntdll userenv ws2_32)
  elseif(APPLE)
    find_library(ZEVRYON_MASSIVEDOC_SECURITY_FRAMEWORK Security REQUIRED)
    find_library(ZEVRYON_MASSIVEDOC_FOUNDATION_FRAMEWORK Foundation REQUIRED)
    find_library(ZEVRYON_MASSIVEDOC_ICONV_LIBRARY iconv REQUIRED)
    target_link_libraries(
      ${target}
      PUBLIC
        "${ZEVRYON_MASSIVEDOC_SECURITY_FRAMEWORK}"
        "${ZEVRYON_MASSIVEDOC_FOUNDATION_FRAMEWORK}"
        "${ZEVRYON_MASSIVEDOC_ICONV_LIBRARY}")
  else()
    find_package(Threads REQUIRED)
    target_link_libraries(
      ${target}
      PUBLIC Threads::Threads dl m rt util)
  endif()

  if(ZEVRYON_RUST_MASSIVEDOC_CODEC_AUTHORITATIVE AND
     NOT TARGET zevryon-massivedoc-codec-authority-tests)
    include(CTest)
    add_executable(
      zevryon-massivedoc-codec-authority-tests
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/massivedoc_descriptor_authority_tests.cpp")
    target_compile_definitions(
      zevryon-massivedoc-codec-authority-tests
      PRIVATE ZEVRYON_MASSIVEDOC_CODEC_AUTHORITY_TEST_HOOKS=1)
    target_link_libraries(
      zevryon-massivedoc-codec-authority-tests PRIVATE ${target})
    zevryon_options(zevryon-massivedoc-codec-authority-tests)

    if(BUILD_TESTING)
      add_test(
        NAME massivedoc-codec-authority
        COMMAND zevryon-massivedoc-codec-authority-tests --positive)
      if(NOT ZEVRYON_RUST_MASSIVEDOC_CODEC_SHADOW_STRICT)
        add_test(
          NAME massivedoc-codec-authority-fault
          COMMAND zevryon-massivedoc-codec-authority-tests --fault)
      endif()
    endif()
  endif()
endfunction()

function(zevryon_configure_rust_unicode_authority)
  if(NOT TARGET zevryon-massivedoc-core)
    message(FATAL_ERROR "Unicode authority requires zevryon-massivedoc-core")
  endif()

  set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/unicode_stream.cpp"
    PROPERTIES HEADER_FILE_ONLY TRUE)
  target_sources(
    zevryon-massivedoc-core
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/unicode_stream_authoritative.cpp")
  target_compile_definitions(
    zevryon-massivedoc-core
    PUBLIC ZEVRYON_UTF8_RUST_AUTHORITATIVE=1)
  if(ZEVRYON_RUST_UNICODE_AUTHORITY_TEST_HOOKS)
    target_compile_definitions(
      zevryon-massivedoc-core
      PRIVATE ZEVRYON_UTF8_RUST_AUTHORITY_TEST_HOOKS=1)
  endif()

  if(BUILD_TESTING AND NOT TARGET zevryon-unicode-authority-tests)
    add_executable(
      zevryon-unicode-authority-tests
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/unicode_stream_authority_tests.cpp")
    target_link_libraries(
      zevryon-unicode-authority-tests PRIVATE zevryon-massivedoc-core)
    zevryon_options(zevryon-unicode-authority-tests)
    add_test(
      NAME unicode-authority-positive
      COMMAND zevryon-unicode-authority-tests --positive)
    if(ZEVRYON_RUST_UNICODE_AUTHORITY_TEST_HOOKS AND
       NOT ZEVRYON_RUST_UNICODE_SHADOW_STRICT)
      foreach(fault output error state reset)
        add_test(
          NAME unicode-authority-fault-${fault}
          COMMAND zevryon-unicode-authority-tests --fault ${fault})
      endforeach()
    endif()
  endif()
endfunction()

if(ZEVRYON_RUST_UNICODE_AUTHORITATIVE)
  cmake_language(DEFER CALL zevryon_configure_rust_unicode_authority)
endif()
