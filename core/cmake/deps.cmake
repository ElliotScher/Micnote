# Third-party dependency pins. Every dependency is declared here with
# micnote_fetch() and made available where it's used with
# FetchContent_MakeAvailable(), so this file is the one place versions change.
#
# GIT_TAG must be a full 40-character commit hash: a branch or tag can move
# under us, a hash can't. The comment on each pin names the release it is.
# A dependency whose git tree can't be built as-is (it needs generated files
# that only its release archive carries) uses micnote_fetch_url() instead:
# the archive's URL plus its SHA-256, which pins it just as firmly.
#
# Offline or local-checkout builds: pass -DFETCHCONTENT_SOURCE_DIR_<NAME>=<dir>
# (e.g. FETCHCONTENT_SOURCE_DIR_GOOGLETEST) to use a directory instead.
include_guard(GLOBAL)
include(FetchContent)

function(micnote_fetch name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "GIT_REPOSITORY;GIT_TAG" "")
  if(NOT arg_GIT_REPOSITORY)
    message(FATAL_ERROR "micnote_fetch(${name}): GIT_REPOSITORY is required")
  endif()
  # CMake regexes have no {n} repetition, hence the separate length check.
  string(LENGTH "${arg_GIT_TAG}" _len)
  if(NOT _len EQUAL 40 OR NOT arg_GIT_TAG MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR
      "micnote_fetch(${name}): GIT_TAG must be a full commit hash, got '${arg_GIT_TAG}'")
  endif()
  # SYSTEM: their headers don't trip our -Werror. EXCLUDE_FROM_ALL: only the
  # targets we link get built.
  FetchContent_Declare(${name}
    GIT_REPOSITORY "${arg_GIT_REPOSITORY}"
    GIT_TAG "${arg_GIT_TAG}"
    GIT_PROGRESS OFF
    SYSTEM
    EXCLUDE_FROM_ALL
    ${arg_UNPARSED_ARGUMENTS})
endfunction()

function(micnote_fetch_url name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "URL;SHA256" "")
  string(LENGTH "${arg_SHA256}" _len)
  if(NOT arg_URL OR NOT _len EQUAL 64 OR NOT arg_SHA256 MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "micnote_fetch_url(${name}): URL and a 64-digit SHA256 are required")
  endif()
  FetchContent_Declare(${name}
    URL "${arg_URL}"
    URL_HASH "SHA256=${arg_SHA256}"
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    SYSTEM
    EXCLUDE_FROM_ALL
    ${arg_UNPARSED_ARGUMENTS})
endfunction()

# v1.18.0
micnote_fetch(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG 063de7e9578f82b369302001269680b4b1553359)

# Runtime only (pb_encode.c, pb_decode.c, pb_common.c); proto/CMakeLists.txt
# builds it. The same commit as Zephyr's nanopb module at NCS v3.4.1
# (nanopb-0.4.9-70-g5499fd4, "1.0.0-dev"), so puck and host run identical
# code. SOURCE_SUBDIR points at a directory that doesn't exist, so
# FetchContent_MakeAvailable() only downloads: nanopb's own CMakeLists.txt
# looks for Python and protoc, which the runtime doesn't need.
micnote_fetch(nanopb
  GIT_REPOSITORY https://github.com/nanopb/nanopb.git
  GIT_TAG 5499fd4c9a478f8139eeb07a82c3b4468d6067f7
  SOURCE_SUBDIR _no_cmake)

# PSA Crypto for the host side of the recording crypto (X25519, HKDF-SHA256,
# AES-256-CCM, SHA-256): the same API and the same LTS branch the firmware
# gets from NCS v3.4.1 (TF-PSA-Crypto 1.1.1 there; 1.1.2 here for its
# security fixes). The release archive, because the git tree needs Python
# code generation (jinja2) to build. core/CMakeLists.txt builds it.
# tf-psa-crypto-1.1.2, sha256 from the release's -sha256sum.txt.
micnote_fetch_url(tfpsacrypto
  URL https://github.com/Mbed-TLS/TF-PSA-Crypto/releases/download/tf-psa-crypto-1.1.2/tf-psa-crypto-1.1.2.tar.bz2
  SHA256 799764c15cebd0d8cc161c71f9601009b2992f471828739fb4d72573b87f8298)

# JSON. Tests only for now (reading proto/testvectors/vectors.json); stage 2
# makes it the C ABI's JSON library. v3.12.0
micnote_fetch(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG 55f93686c01528224f448c19128836e7df245f72)

# The encrypted store (stage 2 §4.3): SQLite with SQLite3MultipleCiphers'
# SQLCipher-compatible encryption, from its release amalgamation (one C
# file). core/cmake/sqlite3mc.cmake builds it. v2.7.1 = SQLite 3.54.1;
# sha256 from the release asset's digest.
micnote_fetch_url(sqlite3mc
  URL https://github.com/utelle/SQLite3MultipleCiphers/releases/download/v2.7.1/sqlite3mc-2.7.1-sqlite-3.54.1-amalgamation.zip
  SHA256 d6bc59678e015c5ac6d3ec5d7b657c351230b36c8052b185e446c9d52ba01af0)

# Opus decoding (stage 2 §4.5), for transcription and playback: the same
# commit the firmware encodes with (firmware/west.yml), v1.6.1.
micnote_fetch(opus
  GIT_REPOSITORY https://github.com/xiph/opus.git
  GIT_TAG 22244de5a79bd1d6d623c32e72bf1954b56235be)

# The OS keychain for micnote-cli (macOS Keychain, Windows Credential Manager,
# libsecret): core/cli/os_keychain.cpp. Main as of 2026-07-03 ("minor fixes
# (#53)"), past v1.3.1: it adds isAvailable(), which tells a Linux machine
# with no Secret Service (headless, CI) apart from a failing one.
micnote_fetch(keychain
  GIT_REPOSITORY https://github.com/hrantzsch/keychain.git
  GIT_TAG f172de882517208e5e44ba1b78a4234acf0814f9)

# Speech recognition, only with -DMICNOTE_ASR=ON (core/src/asr/, the
# micnote_asr target): desktop and the CLI for now, mobile in stage 5.
# v1.9.5.
micnote_fetch(whispercpp
  GIT_REPOSITORY https://github.com/ggml-org/whisper.cpp.git
  GIT_TAG d1be6fde11ac6e0407606b4e42fe72d34add8037)

# The planner's local language model, only with -DMICNOTE_LLM=ON. v0.6.0, the
# release with the same ggml (0.26.0) as whisper.cpp v1.9.5 above: the two
# share whisper's copy, so upgrade them together.
micnote_fetch(llamacpp
  GIT_REPOSITORY https://github.com/ggml-org/llama.cpp.git
  GIT_TAG d81235049384534c167caea52b85a694f6103d14)
