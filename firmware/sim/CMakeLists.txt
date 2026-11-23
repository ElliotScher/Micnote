# A puck simulator on native_sim: the firmware's real recording store,
# crypto and protocol dispatcher, serving the link on a host pseudo-terminal
# so micnote-cli can sync from it with no hardware. See README.md.
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(micnote_sim)

set(MICNOTE_APP_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../app/src)
set(MICNOTE_PROTO_GEN ${CMAKE_CURRENT_SOURCE_DIR}/../../proto/gen)

target_include_directories(app PRIVATE
  ${MICNOTE_APP_SRC}/crypto ${MICNOTE_APP_SRC}/storage ${MICNOTE_APP_SRC}/proto_link
  ${MICNOTE_PROTO_GEN})
target_sources(app PRIVATE
  src/main.c
  ${MICNOTE_APP_SRC}/proto_link/dispatch.c
  ${MICNOTE_APP_SRC}/proto_link/framing.c
  ${MICNOTE_APP_SRC}/storage/recstore.c
  ${MICNOTE_APP_SRC}/storage/mnr.c
  ${MICNOTE_APP_SRC}/crypto/wrap.c
  ${MICNOTE_APP_SRC}/crypto/aead.c
  ${MICNOTE_PROTO_GEN}/micnote.pb.c
)
