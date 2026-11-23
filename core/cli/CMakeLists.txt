# The commands as a library, so the tests can run them against a fake puck;
# micnote-cli itself is a thin main() over it.
add_library(micnote_cli_lib STATIC commands.cpp)
target_include_directories(micnote_cli_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(micnote_cli_lib
  PUBLIC micnote::micnote micnote::impl
  PRIVATE micnote::warnings)

# `transcribe`, when built with -DMICNOTE_ASR=ON.
if(TARGET micnote_asr)
  target_sources(micnote_cli_lib PRIVATE transcribe.cpp)
  target_link_libraries(micnote_cli_lib PUBLIC micnote::asr)
endif()

add_executable(micnote-cli main.cpp)
target_link_libraries(micnote-cli PRIVATE micnote_cli_lib micnote::warnings)
