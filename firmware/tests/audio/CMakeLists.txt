cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(micnote_audio_test)

set(MICNOTE_APP_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../app/src)

target_include_directories(app PRIVATE ${MICNOTE_APP_SRC}/audio ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_sources(app PRIVATE
  src/preproc.c
  src/encoder.c
  src/pipeline.c
  ${MICNOTE_APP_SRC}/audio/preproc.c
  ${MICNOTE_APP_SRC}/audio/encoder.c
  ${MICNOTE_APP_SRC}/audio/pipeline.c
)
