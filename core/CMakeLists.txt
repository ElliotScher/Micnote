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
# Shared for desktop and Android (loaded with dart:ffi); static on iOS, where
# Flutter links it into the app from an XCFramework.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(_micnote_shared_default OFF)
else()
  set(_micnote_shared_default ON)
endif()
option(MICNOTE_SHARED "Build libmicnote as a shared library" ${_micnote_shared_default})

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(sanitizers)
include(coverage)
include(warnings)
include(deps)

add_subdirectory(../proto "${CMAKE_CURRENT_BINARY_DIR}/proto")

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
target_link_libraries(micnote PRIVATE micnote::proto micnote::warnings)
set_target_properties(micnote PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR})

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
