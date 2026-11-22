cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(micnote_framing_test)

# The app's framing module, built against the committed nanopb output in
# proto/gen and Zephyr's nanopb runtime (CONFIG_NANOPB).
set(MICNOTE_PROTO_GEN ${CMAKE_CURRENT_SOURCE_DIR}/../../../proto/gen)
set(MICNOTE_APP_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../app/src)

target_include_directories(app PRIVATE ${MICNOTE_PROTO_GEN} ${MICNOTE_APP_SRC}/proto_link)
target_sources(app PRIVATE
  src/main.c
  ${MICNOTE_APP_SRC}/proto_link/framing.c
  ${MICNOTE_PROTO_GEN}/micnote.pb.c
)
