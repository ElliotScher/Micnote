# -DMICNOTE_COVERAGE=ON (the `coverage` preset): gcc's --coverage on
# everything; gcovr.cfg at the repo root picks out Micnote's own code and sets
# the 100% gate CI enforces.
include_guard(GLOBAL)

option(MICNOTE_COVERAGE "Instrument for gcov/gcovr (gcc only)" OFF)

if(MICNOTE_COVERAGE)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR "MICNOTE_COVERAGE needs gcc (gcovr reads gcov data)")
  endif()
  # No -O flag needed: the preset builds Debug, which is unoptimized.
  add_compile_options(--coverage)
  add_link_options(--coverage)
endif()
