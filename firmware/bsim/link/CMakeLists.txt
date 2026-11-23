# BLE end to end in BabbleSim (see ../README.md): the puck's real BLE module,
# dispatcher, host slots and recording store, against a simulated host.
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(micnote_bsim_link)

add_subdirectory(${ZEPHYR_BASE}/tests/bsim/babblekit babblekit)
target_link_libraries(app PRIVATE babblekit)

set(MICNOTE_APP_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../app/src)
set(MICNOTE_PROTO_GEN ${CMAKE_CURRENT_SOURCE_DIR}/../../../proto/gen)

target_include_directories(app PRIVATE
  ${MICNOTE_APP_SRC}/ble ${MICNOTE_APP_SRC}/crypto ${MICNOTE_APP_SRC}/storage
  ${MICNOTE_APP_SRC}/proto_link ${MICNOTE_PROTO_GEN})
target_sources(app PRIVATE
  src/main.c
  src/puck.c
  src/host.c
  src/stranger.c
  src/host_link.c
  ${MICNOTE_APP_SRC}/ble/ble.c
  ${MICNOTE_APP_SRC}/ble/adv.c
  ${MICNOTE_APP_SRC}/ble/gatt_svc.c
  ${MICNOTE_APP_SRC}/ble/l2cap.c
  ${MICNOTE_APP_SRC}/proto_link/dispatch.c
  ${MICNOTE_APP_SRC}/proto_link/framing.c
  ${MICNOTE_APP_SRC}/storage/recstore.c
  ${MICNOTE_APP_SRC}/storage/mnr.c
  ${MICNOTE_APP_SRC}/storage/hosts.c
  ${MICNOTE_APP_SRC}/crypto/wrap.c
  ${MICNOTE_APP_SRC}/crypto/aead.c
  ${MICNOTE_APP_SRC}/crypto/keys.c
  ${MICNOTE_APP_SRC}/crypto/pairing.c
  ${MICNOTE_PROTO_GEN}/micnote.pb.c
)
zephyr_include_directories(
  ${BSIM_COMPONENTS_PATH}/libUtilv1/src/
  ${BSIM_COMPONENTS_PATH}/libPhyComv1/src/
)
