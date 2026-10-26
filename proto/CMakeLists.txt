# micnote::proto: the committed nanopb output plus the nanopb runtime, for the
# host side. The firmware compiles the same gen/ files against Zephyr's nanopb
# module instead (firmware/app/CMakeLists.txt). Pins: core/cmake/deps.cmake.
FetchContent_MakeAvailable(nanopb)

add_library(micnote_proto STATIC
  gen/micnote.pb.c
  "${nanopb_SOURCE_DIR}/pb_common.c"
  "${nanopb_SOURCE_DIR}/pb_decode.c"
  "${nanopb_SOURCE_DIR}/pb_encode.c")
add_library(micnote::proto ALIAS micnote_proto)
target_include_directories(micnote_proto PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/gen")
# SYSTEM: generated and third-party headers stay out of our warnings.
target_include_directories(micnote_proto SYSTEM PUBLIC "${nanopb_SOURCE_DIR}")
# Linked into the shared libmicnote.
set_target_properties(micnote_proto PROPERTIES POSITION_INDEPENDENT_CODE ON)
