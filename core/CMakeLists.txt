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
# Speech recognition (whisper.cpp): the micnote_asr target and
# `micnote-cli transcribe`. Off by default, so the app's builds (mobile
# included) and the coverage job don't carry it; stage 5 turns it on for
# phones.
option(MICNOTE_ASR "Build micnote_asr (whisper.cpp) and micnote-cli transcribe" OFF)
# micnote-cli keeps its keys in the OS keychain (hrantzsch/keychain; on Linux
# it needs libsecret-1 and glib-2.0, which bootstrap installs). Off: files only.
option(MICNOTE_KEYCHAIN "micnote-cli uses the OS keychain" ON)
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
# its release archive's pre-generated sources, with its default config plus
# cmake/tfpsa/micnote_crypto_config.h (thread safety for the job queue).
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(GEN_FILES OFF CACHE BOOL "" FORCE)
set(TF_PSA_CRYPTO_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_STATIC_TF_PSA_CRYPTO_LIBRARY ON CACHE BOOL "" FORCE)
set(USE_SHARED_TF_PSA_CRYPTO_LIBRARY OFF CACHE BOOL "" FORCE)
set(TF_PSA_CRYPTO_USER_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/tfpsa/micnote_crypto_config.h"
  CACHE FILEPATH "" FORCE)
# threading_alt.h, for MBEDTLS_THREADING_ALT on Windows: mbedtls/threading.h
# includes it from every TF-PSA-Crypto source, and the library compiles them
# in internal targets (builtin, ...) that don't see tfpsacrypto's include
# directories. So the directory's include path carries it while those
# targets are created, then goes back to what it was.
get_directory_property(micnote_saved_includes INCLUDE_DIRECTORIES)
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/cmake/tfpsa")
FetchContent_MakeAvailable(tfpsacrypto)
set_directory_properties(PROPERTIES INCLUDE_DIRECTORIES "${micnote_saved_includes}")
# And for the library's users (micnote_impl, the tests).
target_include_directories(tfpsacrypto PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/cmake/tfpsa")
if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(tfpsacrypto PUBLIC Threads::Threads)
endif()
# Linked into the shared libmicnote.
set_target_properties(tfpsacrypto PROPERTIES POSITION_INDEPENDENT_CODE ON)

include(sqlite3mc)
# The C ABI's JSON (stage 2 §4.2 allows glaze or nlohmann; nlohmann builds on
# every compiler CI uses, glaze would need proving on MSVC and Apple first).
FetchContent_MakeAvailable(nlohmann_json)

# libopus for decoding recordings (the library only: no programs, tests or
# install rules), static and position-independent for the shared libmicnote.
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
# MSVC: opus sets its own CMAKE_MSVC_RUNTIME_LIBRARY (the DLL runtime unless
# OPUS_STATIC_RUNTIME), so it follows ours: the windows-msvc preset links the
# static runtime, the Flutter app's build the default DLL one. A mismatch
# fails at link time (__imp_floor unresolved in micnote.dll).
if(MSVC)
  if(DEFINED CMAKE_MSVC_RUNTIME_LIBRARY AND NOT CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "DLL$")
    set(OPUS_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
  else()
    set(OPUS_STATIC_RUNTIME OFF CACHE BOOL "" FORCE)
  endif()
endif()
FetchContent_MakeAvailable(opus)
set_target_properties(opus PROPERTIES POSITION_INDEPENDENT_CODE ON)

# The store's migrations, embedded: each store/migrations/*.sql becomes a
# {name, raw string} entry in a generated header schema.cpp includes.
file(GLOB MICNOTE_MIGRATIONS CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/store/migrations/*.sql")
list(SORT MICNOTE_MIGRATIONS)
set(_migrations_inc "")
foreach(_sql IN LISTS MICNOTE_MIGRATIONS)
  get_filename_component(_name "${_sql}" NAME)
  file(READ "${_sql}" _body)
  string(APPEND _migrations_inc
    "    Migration{.name = \"${_name}\", .sql = R\"mnsql(${_body})mnsql\"},\n")
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/store/migrations.inc"
  CONTENT "${_migrations_inc}" @ONLY)

# The model registry's pinned manifest (models/manifest.json), embedded the
# same way, so the URLs and hashes always ship with the code that reads them.
set(MICNOTE_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/../models/manifest.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MICNOTE_MANIFEST}")
file(READ "${MICNOTE_MANIFEST}" _manifest)
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/models/manifest.inc"
  CONTENT "R\"mnjson(${_manifest})mnjson\"\n" @ONLY)

# micnote::impl: libmicnote's internals (protocol, crypto, recordings, and
# later the store, ...). A static library of its own so tests and fuzzers
# can reach symbols the shared library hides; libmicnote links it in.
add_library(micnote_impl STATIC
  src/protocol/framing.cpp
  src/protocol/client.cpp
  src/protocol/session.cpp
  src/transport/usb_serial.cpp
  src/crypto/psa.cpp
  src/crypto/pairing.cpp
  src/recording/mnr.cpp
  src/recording/reader.cpp
  src/recording/ogg_writer.cpp
  src/store/db.cpp
  src/store/schema.cpp
  src/store/repos.cpp
  src/secrets/secrets.cpp
  src/secrets/keychain.cpp
  src/time/clock_fix.cpp
  src/api/context.cpp
  src/audio/opus_decode.cpp
  src/asr/transcribe.cpp
  src/models/registry.cpp
  $<$<BOOL:${WIN32}>:src/crypto/threading_win32.cpp>
  src/jobs/queue.cpp
  src/jobs/sync_job.cpp
  src/jobs/transcribe_job.cpp)
add_library(micnote::impl ALIAS micnote_impl)
target_include_directories(micnote_impl PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(micnote_impl PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
# The C ABI's types (api/ implements micnote.h).
target_include_directories(micnote_impl PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(micnote_impl PUBLIC micnote::proto tfpsacrypto micnote::sqlite
  PRIVATE micnote::warnings nlohmann_json::nlohmann_json opus)
set_target_properties(micnote_impl PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MICNOTE_SHARED)
  add_library(micnote SHARED)
else()
  add_library(micnote STATIC)
  target_compile_definitions(micnote PUBLIC MICNOTE_STATIC)
endif()
add_library(micnote::micnote ALIAS micnote)
target_sources(micnote PRIVATE src/version.cpp src/api/abi.cpp)
target_include_directories(micnote PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(micnote
  PRIVATE MICNOTE_BUILDING "MICNOTE_VERSION_STRING=\"${PROJECT_VERSION}\"")
target_link_libraries(micnote PRIVATE micnote::impl micnote::proto micnote::warnings)
if(MICNOTE_VERSIONED_SONAME)
  set_target_properties(micnote PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})
endif()

if(MICNOTE_ASR)
  # whisper.cpp as a static library, CPU backend by default. The GPU
  # backends of stage 2 §4.2 (Metal, Vulkan, CUDA) are its own GGML_* options,
  # passed on the configure line per platform.
  set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(whispercpp)
  add_library(micnote_asr STATIC src/asr/whisper_engine.cpp)
  add_library(micnote::asr ALIAS micnote_asr)
  target_link_libraries(micnote_asr PUBLIC micnote::impl PRIVATE whisper micnote::warnings)
  target_compile_definitions(micnote_asr PUBLIC MICNOTE_HAVE_ASR=1)
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
