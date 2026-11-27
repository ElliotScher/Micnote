# llama.cpp v0.6.0's src/llama-memory-hybrid-idx.cpp defines two structs that
# its classes hold in std::unique_ptr only after the constructors that may
# have to destroy them, which libc++ (Apple's) and recent libstdc++ refuse
# ("invalid application of 'sizeof' to an incomplete type"). This moves them
# above the first constructor. Run as FetchContent's PATCH_COMMAND, in the
# source directory; running it twice changes nothing.
set(_file "src/llama-memory-hybrid-idx.cpp")
file(READ "${_file}" _src)
set(_begin "// Which cells of a sequence make up which pool, for the whole cache.\n")
set(_end "    bool     cache_safe  = true;\n};\n")
set(_ctor "//\n// llama_memory_hybrid_idx\n//\n\nllama_memory_hybrid_idx::llama_memory_hybrid_idx(")
string(FIND "${_src}" "${_begin}" _b)
string(FIND "${_src}" "${_ctor}" _c)
if(_b EQUAL -1 OR _c EQUAL -1)
  message(FATAL_ERROR "llama-kpool-order: ${_file} isn't what this patch expects; update or drop it")
endif()
if(_b LESS _c)
  return()  # already patched
endif()
string(SUBSTRING "${_src}" ${_b} -1 _rest)
string(FIND "${_rest}" "${_end}" _e)
if(_e EQUAL -1)
  message(FATAL_ERROR "llama-kpool-order: no end of the structs in ${_file}")
endif()
string(LENGTH "${_end}" _end_len)
math(EXPR _len "${_e} + ${_end_len}")
string(SUBSTRING "${_src}" ${_b} ${_len} _structs)
string(REPLACE "${_structs}" "" _src "${_src}")
string(REPLACE "${_ctor}" "${_structs}\n${_ctor}" _src "${_src}")
file(WRITE "${_file}" "${_src}")
