cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(micnote_proto_test)

# The committed nanopb output in proto/gen, shared with libmicnote. Zephyr's
# nanopb module (CONFIG_NANOPB) supplies the runtime; nothing is regenerated.
set(MICNOTE_PROTO_GEN ${CMAKE_CURRENT_SOURCE_DIR}/../../../proto/gen)

target_include_directories(app PRIVATE ${MICNOTE_PROTO_GEN})
target_sources(app PRIVATE
  src/main.c
  ${MICNOTE_PROTO_GEN}/micnote.pb.c
)
