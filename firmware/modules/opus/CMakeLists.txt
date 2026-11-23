# libopus as a Zephyr library. Opus's own CMakeLists builds for a host and
# probes the compiler; this builds just the fixed-point codec from the same
# source lists (the *_sources.mk files), with the configuration as defines.
if(NOT CONFIG_OPUS)
  return()
endif()

set(MICNOTE_OPUS_DIR "${WEST_TOPDIR}/external/modules/lib/opus"
    CACHE PATH "xiph/opus checkout (west project 'opus' in firmware/west.yml)")
if(NOT EXISTS "${MICNOTE_OPUS_DIR}/src/opus_encoder.c")
  message(FATAL_ERROR "libopus not found at ${MICNOTE_OPUS_DIR}; run 'west update opus'")
endif()

include("${MICNOTE_OPUS_DIR}/cmake/OpusFunctions.cmake")
get_opus_sources(OPUS_SOURCES "${MICNOTE_OPUS_DIR}/opus_sources.mk" opus_sources)
get_opus_sources(CELT_SOURCES "${MICNOTE_OPUS_DIR}/celt_sources.mk" celt_sources)
get_opus_sources(SILK_SOURCES "${MICNOTE_OPUS_DIR}/silk_sources.mk" silk_sources)
get_opus_sources(SILK_SOURCES_FIXED "${MICNOTE_OPUS_DIR}/silk_sources.mk" silk_sources_fixed)

zephyr_library_named(opus)
foreach(src IN LISTS opus_sources celt_sources silk_sources silk_sources_fixed)
  zephyr_library_sources("${MICNOTE_OPUS_DIR}/${src}")
endforeach()

# The public header directory is the only one the app sees.
zephyr_include_directories("${MICNOTE_OPUS_DIR}/include")
zephyr_library_include_directories(
  "${MICNOTE_OPUS_DIR}"
  "${MICNOTE_OPUS_DIR}/celt"
  "${MICNOTE_OPUS_DIR}/silk"
  "${MICNOTE_OPUS_DIR}/silk/fixed"
)

# - FIXED_POINT + DISABLE_FLOAT_API: integer codec, no float entry points.
# - VAR_ARRAYS: scratch arrays on the caller's stack (no allocator, no
#   global pseudo-stack), so the encoder thread's stack holds them.
# - OPUS_BUILD: the sources' own switch for building the library itself.
# - PACKAGE_VERSION: what opus_get_version_string() reports; autotools and
#   Opus's CMake derive it from git, so here it follows the west.yml pin.
zephyr_library_compile_definitions(
  OPUS_BUILD
  PACKAGE_VERSION="1.6.1"
  FIXED_POINT
  DISABLE_FLOAT_API
  VAR_ARRAYS
)
if(CONFIG_OPUS_ARM_INLINE_ASM)
  zephyr_library_compile_definitions(
    OPUS_ARM_INLINE_ASM
    OPUS_ARM_INLINE_EDSP
    OPUS_ARM_INLINE_MEDIA
  )
endif()

# Third-party code: built at -O2 even in debug builds (the codec at -Og would
# say nothing about the CPU budget), and without the warnings Zephyr turns
# on for its own code.
zephyr_library_compile_options(-O2 -w)
