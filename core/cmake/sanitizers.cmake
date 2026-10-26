# Sanitizer and fuzzing switches. They apply to everything configured after
# this file, dependencies included: ASan reports false positives when only
# part of a program is instrumented.
#
#   -DMICNOTE_SANITIZERS="address;undefined"   (gcc or clang)
#   -DMICNOTE_FUZZ=ON                          (clang only; builds core/fuzz)
include_guard(GLOBAL)

set(MICNOTE_SANITIZERS "" CACHE STRING
  "Sanitizers to build with, e.g. address;undefined")
option(MICNOTE_FUZZ "Build the libFuzzer targets in core/fuzz (clang only)" OFF)

if(MICNOTE_SANITIZERS)
  if(MSVC)
    message(FATAL_ERROR "MICNOTE_SANITIZERS: use the gcc/clang presets")
  endif()
  list(JOIN MICNOTE_SANITIZERS "," _micnote_san)
  add_compile_options(-fsanitize=${_micnote_san} -fno-omit-frame-pointer
                      -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${_micnote_san})
endif()

if(MICNOTE_FUZZ)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "MICNOTE_FUZZ needs clang (libFuzzer): set CC/CXX to clang")
  endif()
  # Coverage instrumentation everywhere; only the fuzz executables link
  # libFuzzer's main() (-fsanitize=fuzzer, in core/fuzz).
  add_compile_options(-fsanitize=fuzzer-no-link)
endif()
