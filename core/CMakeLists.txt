cmake_minimum_required(VERSION 3.28)
# Versions move in lockstep with the firmware and app (PLAN.md section 5).
project(micnote VERSION 0.0.1 DESCRIPTION "Micnote host core" LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# No C++ modules yet; scanning would need clang-scan-deps on clang builds.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
# Only mn_* (MN_API) is exported from the shared library.
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# Executables next to the DLL, so tests and the CLI run in place on Windows.
# An embedder (Flutter) that sets its own output directory keeps it.
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
endif()

option(MICNOTE_BUILD_TESTS "Build the libmicnote unit tests" OFF)
option(MICNOTE_BUILD_CLI "Build micnote-cli" OFF)
# Shared on every platform: the Flutter app's build hook
# (app/packages/micnote_core/hook/build.dart) bundles it as a dynamic library,
# wrapped in a framework on iOS and macOS, and dart:ffi loads it.
option(MICNOTE_SHARED "Build libmicnote as a shared library" ON)
# libmicnote.so.0.0.1 + symlinks for system installs. The build hook turns this
# off: Android only packages plain lib<name>.so files, and an app bundle has
# no use for the symlinks.
option(MICNOTE_VERSIONED_SONAME "Give the shared library a versioned file name and soname" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(sanitizers)
include(coverage)
include(warnings)
include(deps)

add_subdirectory(../proto "${CMAKE_CURRENT_BINARY_DIR}/proto")

# TF-PSA-Crypto (pinned in cmake/deps.cmake): the static library only, from
# its release archive's pre-generated sources, with its default config.
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(GEN_FILES OFF CACHE BOOL "" FORCE)
set(TF_PSA_CRYPTO_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_STATIC_TF_PSA_CRYPTO_LIBRARY ON CACHE BOOL "" FORCE)
set(USE_SHARED_TF_PSA_CRYPTO_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(tfpsacrypto)
# Linked into the shared libmicnote.
set_target_properties(tfpsacrypto PROPERTIES POSITION_INDEPENDENT_CODE ON)

# micnote::impl: libmicnote's internals (protocol, crypto, recordings, and
# later the store, ...). A static library of its own so tests and fuzzers
# can reach symbols the shared library hides; libmicnote links it in.
add_library(micnote_impl STATIC
  src/protocol/framing.cpp
  src/crypto/psa.cpp
  src/recording/mnr.cpp
  src/recording/reader.cpp)
add_library(micnote::impl ALIAS micnote_impl)
target_include_directories(micnote_impl PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(micnote_impl PUBLIC micnote::proto tfpsacrypto PRIVATE micnote::warnings)
set_target_properties(micnote_impl PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MICNOTE_SHARED)
  add_library(micnote SHARED)
else()
  add_library(micnote STATIC)
  target_compile_definitions(micnote PUBLIC MICNOTE_STATIC)
endif()
add_library(micnote::micnote ALIAS micnote)
target_sources(micnote PRIVATE src/version.cpp)
target_include_directories(micnote PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(micnote
  PRIVATE MICNOTE_BUILDING "MICNOTE_VERSION_STRING=\"${PROJECT_VERSION}\"")
target_link_libraries(micnote PRIVATE micnote::impl micnote::proto micnote::warnings)
if(MICNOTE_VERSIONED_SONAME)
  set_target_properties(micnote PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})
endif()

if(MICNOTE_BUILD_CLI)
  add_subdirectory(cli)
endif()
if(MICNOTE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(MICNOTE_FUZZ)
  add_subdirectory(fuzz)
endif()
