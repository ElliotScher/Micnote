# micnote::warnings: the warning set for Micnote's own code. Link it PRIVATE;
# third-party and generated code doesn't get it.
include_guard(GLOBAL)

option(MICNOTE_WERROR "Treat compiler warnings as errors" OFF)

add_library(micnote_warnings INTERFACE)
add_library(micnote::warnings ALIAS micnote_warnings)

if(MSVC)
  target_compile_options(micnote_warnings INTERFACE
    /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor
    $<$<BOOL:${MICNOTE_WERROR}>:/WX>)
else()
  target_compile_options(micnote_warnings INTERFACE
    -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
    -Wcast-align -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough
    # C++ only (the C ABI test, abi_test.c, gets the rest).
    $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual>
    $<$<BOOL:${MICNOTE_WERROR}>:-Werror>)
endif()
