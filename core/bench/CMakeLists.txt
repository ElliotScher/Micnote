# micnote-wer, the accuracy and speed harness (stage 2 PR 5), in builds with
# -DMICNOTE_ASR=ON, and micnote-eval, the planner's (stage 3 PR 3), with
# -DMICNOTE_LLM=ON. The scoring is a library, tested in micnote_tests.
add_library(micnote_wer_lib STATIC wer.cpp plan_score.cpp)
target_include_directories(micnote_wer_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(micnote_wer_lib PUBLIC nlohmann_json::nlohmann_json PRIVATE micnote::warnings)
if(TARGET micnote_asr)
  # miniaudio isn't written to our warnings: its own target, without them.
  add_library(micnote_audio_file STATIC audio_file.cpp)
  target_include_directories(micnote_audio_file SYSTEM PRIVATE "${whispercpp_SOURCE_DIR}/examples")
  if(NOT WIN32)
    target_link_libraries(micnote_audio_file PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  endif()
  add_executable(micnote-wer main.cpp)
  target_link_libraries(micnote-wer PRIVATE
    micnote_wer_lib micnote_audio_file micnote::asr micnote::impl micnote::warnings opus)
endif()
if(TARGET micnote_llm)
  add_executable(micnote-eval eval_main.cpp)
  target_link_libraries(micnote-eval PRIVATE
    micnote_wer_lib micnote::llm micnote::impl micnote::warnings)
endif()
