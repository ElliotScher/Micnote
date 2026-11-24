# The commands as a library, so the tests can run them against a fake puck;
# micnote-cli itself is a thin main() over it.
add_library(micnote_cli_lib STATIC commands.cpp keys.cpp models.cpp local.cpp db.cpp run.cpp plan.cpp)
target_include_directories(micnote_cli_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(micnote_cli_lib
  PUBLIC micnote::micnote micnote::impl
  PRIVATE micnote::warnings nlohmann_json::nlohmann_json)

# `transcribe`, when built with -DMICNOTE_ASR=ON.
if(TARGET micnote_asr)
  target_sources(micnote_cli_lib PRIVATE transcribe.cpp)
  target_link_libraries(micnote_cli_lib PUBLIC micnote::asr)
endif()

# The language model for `plan` and `run`, when built with -DMICNOTE_LLM=ON.
if(TARGET micnote_llm)
  target_link_libraries(micnote_cli_lib PUBLIC micnote::llm)
endif()

# The OS keychain (stage 2 §4.6), in micnote-cli and its tests only:
# libmicnote, and so the app, stays free of it (the app hands the core its
# secrets through the C ABI), and no other build needs libsecret.
add_executable(micnote-cli main.cpp)
if(MICNOTE_KEYCHAIN)
  if(NOT WIN32 AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(MICNOTE_LIBSECRET libsecret-1 glib-2.0)
    if(NOT MICNOTE_LIBSECRET_FOUND)
      message(FATAL_ERROR
        "micnote-cli's keychain needs libsecret-1 and glib-2.0 (libsecret-1-dev on Debian/Ubuntu, "
        "libsecret-devel on Fedora; scripts/bootstrap.sh installs them), or configure with "
        "-DMICNOTE_KEYCHAIN=OFF to keep keys in files")
    endif()
  endif()
  set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
  # Static whatever BUILD_SHARED_LIBS says (whisper.cpp's build turns it on):
  # the library doesn't pick, and a shared one built with our hidden
  # visibility would export nothing.
  set(_micnote_shared "${BUILD_SHARED_LIBS}")
  set(BUILD_SHARED_LIBS OFF)
  FetchContent_MakeAvailable(keychain)
  set(BUILD_SHARED_LIBS "${_micnote_shared}")
  add_library(micnote_os_keychain STATIC os_keychain.cpp)
  target_include_directories(micnote_os_keychain PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(micnote_os_keychain PUBLIC micnote::impl PRIVATE keychain micnote::warnings)
  target_compile_definitions(micnote_os_keychain PUBLIC MICNOTE_HAVE_KEYCHAIN=1)
  target_link_libraries(micnote-cli PRIVATE micnote_os_keychain)
endif()
target_link_libraries(micnote-cli PRIVATE micnote_cli_lib micnote::warnings)
