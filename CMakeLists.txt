# Workspace entry point for CMakePresets.json. libmicnote itself lives in
# core/ and is self-contained, so app/native can add_subdirectory(core)
# without this file.
cmake_minimum_required(VERSION 3.28)
project(micnote_workspace LANGUAGES NONE)

# On for a standalone build; core/ defaults them off for embedders (Flutter).
option(MICNOTE_BUILD_TESTS "Build the libmicnote unit tests" ON)
option(MICNOTE_BUILD_CLI "Build micnote-cli" ON)

# ctest only reads the top-level directory's test list.
if(MICNOTE_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(core)
