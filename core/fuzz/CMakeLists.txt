# libFuzzer targets (the `fuzz` preset); stage 7 runs them all on a schedule.
function(micnote_add_fuzzer name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE micnote::impl micnote::proto micnote::warnings)
  target_link_options(${name} PRIVATE -fsanitize=fuzzer)
endfunction()

micnote_add_fuzzer(fuzz_device_info_decode fuzz_device_info_decode.cpp)
micnote_add_fuzzer(fuzz_frame_decode fuzz_frame_decode.cpp)
micnote_add_fuzzer(fuzz_recording_reader fuzz_recording_reader.cpp)
