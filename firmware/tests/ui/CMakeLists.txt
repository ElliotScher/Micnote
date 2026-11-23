cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(micnote_ui_test)

set(MICNOTE_APP_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../app/src)

target_include_directories(app PRIVATE ${MICNOTE_APP_SRC}/ui)
target_sources(app PRIVATE
  src/gestures.c
  src/patterns.c
  ${MICNOTE_APP_SRC}/ui/gestures.c
  ${MICNOTE_APP_SRC}/ui/patterns.c
)
