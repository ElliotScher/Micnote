# libFuzzer targets (the `fuzz` preset). Stage 1 adds fuzz_frame_decode for
# the real link framing; stage 7 runs them all on a schedule.
function(micnote_add_fuzzer name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE micnote::proto micnote::warnings)
  target_link_options(${name} PRIVATE -fsanitize=fuzzer)
endfunction()

micnote_add_fuzzer(fuzz_device_info_decode fuzz_device_info_decode.cpp)
