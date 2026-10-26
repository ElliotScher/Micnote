# Third-party dependency pins. Every dependency is declared here with
# micnote_fetch() and made available where it's used with
# FetchContent_MakeAvailable(), so this file is the one place versions change.
#
# GIT_TAG must be a full 40-character commit hash: a branch or tag can move
# under us, a hash can't. The comment on each pin names the release it is.
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
