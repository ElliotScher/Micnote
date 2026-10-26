set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
# Follow CMAKE_MSVC_RUNTIME_LIBRARY (the windows-msvc preset's static CRT)
# instead of googletest's own CRT flag rewriting.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
include(GoogleTest)

add_executable(micnote_tests
  version_test.cpp
  proto_test.cpp)
target_link_libraries(micnote_tests PRIVATE
  micnote::micnote micnote::proto micnote::warnings GTest::gtest_main)
target_compile_definitions(micnote_tests
  PRIVATE "MICNOTE_VERSION_STRING=\"${PROJECT_VERSION}\"")
# PRE_TEST: list the tests when ctest runs, not at build time, so building
# never needs to execute a test binary (cross builds, missing runtime DLLs).
gtest_discover_tests(micnote_tests DISCOVERY_MODE PRE_TEST)

# micnote-cli is checked by its output. PASS_REGULAR_EXPRESSION ignores the
# exit code, so each test matches text only the intended path prints.
if(TARGET micnote-cli)
  function(micnote_cli_test name expected)
    add_test(NAME cli.${name} COMMAND micnote-cli ${ARGN})
    set_tests_properties(cli.${name} PROPERTIES PASS_REGULAR_EXPRESSION "${expected}")
  endfunction()

  set(version_out "^micnote-cli ${PROJECT_VERSION}\n$")
  set(usage_out "^usage: micnote-cli")
  micnote_cli_test(version "${version_out}" --version)
  micnote_cli_test(version_short "${version_out}" -V)
  micnote_cli_test(help "${usage_out}" --help)
  micnote_cli_test(help_short "${usage_out}" -h)
  micnote_cli_test(bad_arg "${usage_out}" --bogus)
  micnote_cli_test(too_many_args "${usage_out}" --version --help)
  micnote_cli_test(no_args "${usage_out}")
endif()
