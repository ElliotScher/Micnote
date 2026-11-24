set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
# Follow CMAKE_MSVC_RUNTIME_LIBRARY (the windows-msvc preset's static CRT)
# instead of googletest's own CRT flag rewriting.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)
include(GoogleTest)

add_executable(micnote_tests
  version_test.cpp
  proto_test.cpp
  framing_test.cpp
  recording_test.cpp
  ogg_test.cpp
  pairing_test.cpp
  store_test.cpp
  secrets_test.cpp
  keychain_test.cpp
  clock_fix_test.cpp
  context_test.cpp
  jobs_test.cpp
  abi_test.cpp
  abi_test.c
  opus_decode_test.cpp
  asr_test.cpp
  models_test.cpp
  wer_test.cpp
  plan_score_test.cpp
  planner_schema_test.cpp
  planner_grammar_test.cpp
  planner_test.cpp
  client_test.cpp
  usb_serial_test.cpp)
target_link_libraries(micnote_tests PRIVATE
  micnote::micnote micnote::impl micnote::proto micnote::warnings micnote_wer_lib GTest::gtest_main
  nlohmann_json::nlohmann_json)
# proto/testvectors: written by the firmware's recstore, read back here.
target_compile_definitions(micnote_tests
  PRIVATE "MICNOTE_VERSION_STRING=\"${PROJECT_VERSION}\""
          "MICNOTE_TESTVECTORS_DIR=\"${PROJECT_SOURCE_DIR}/../proto/testvectors\"")
# PRE_TEST: list the tests when ctest runs, not at build time, so building
# never needs to execute a test binary (cross builds, missing runtime DLLs).
# The CLI's commands, when the CLI is built.
if(TARGET micnote_cli_lib)
  target_sources(micnote_tests PRIVATE cli_test.cpp)
  target_link_libraries(micnote_tests PRIVATE micnote_cli_lib)
endif()
# The real OS keychain, with micnote-cli's backend for it.
if(TARGET micnote_os_keychain)
  target_sources(micnote_tests PRIVATE os_keychain_test.cpp)
  target_link_libraries(micnote_tests PRIVATE micnote_os_keychain)
endif()
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

# Speech recognition, in builds with -DMICNOTE_ASR=ON: its own executable,
# so CI's asr job builds only this (and the CLI) at -O2. It needs a model:
# MICNOTE_TEST_MODEL=/path/to/ggml-tiny.en.bin, or the tests skip.
if(TARGET micnote_asr)
  add_executable(micnote_asr_tests asr_whisper_test.cpp)
  target_link_libraries(micnote_asr_tests PRIVATE
    micnote::asr micnote::impl micnote::warnings GTest::gtest_main opus nlohmann_json::nlohmann_json)
  if(TARGET micnote_cli_lib)
    target_link_libraries(micnote_asr_tests PRIVATE micnote_cli_lib)
  endif()
  target_compile_definitions(micnote_asr_tests PRIVATE
    "MICNOTE_WHISPER_SAMPLES=\"${whispercpp_SOURCE_DIR}/samples\""
    "MICNOTE_WHISPER_VAD_MODEL=\"${whispercpp_SOURCE_DIR}/models/for-tests-silero-v6.2.0-ggml.bin\""
    "MICNOTE_TESTVECTORS_DIR=\"${PROJECT_SOURCE_DIR}/../proto/testvectors\"")
  gtest_discover_tests(micnote_asr_tests DISCOVERY_MODE PRE_TEST)
endif()

# The local language model, in builds with -DMICNOTE_LLM=ON. It needs a model:
# MICNOTE_TEST_LLM=/path/to/instruct.gguf, or the tests skip.
if(TARGET micnote_llm)
  add_executable(micnote_llm_tests llm_test.cpp)
  target_link_libraries(micnote_llm_tests PRIVATE
    micnote::llm micnote::impl micnote::warnings GTest::gtest_main nlohmann_json::nlohmann_json)
  gtest_discover_tests(micnote_llm_tests DISCOVERY_MODE PRE_TEST)
endif()
