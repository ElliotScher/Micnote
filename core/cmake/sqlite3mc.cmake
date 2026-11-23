# micnote::sqlite: SQLite3MultipleCiphers (pinned in deps.cmake), built
# from its amalgamation as a static, position-independent library.
#
# - SQLCipher (v4-compatible) is the default cipher (stage 2 §4.3).
# - FTS5 for transcript search; threadsafe; no extension loading; no
#   double-quoted string literals (a classic SQL foot-gun).
# - AES in software (SQLITE3MC_OMIT_AES_HARDWARE_SUPPORT): the hardware
#   paths need per-CPU intrinsics flags on every toolchain the app builds
#   with (iOS, Android, MSVC), and the database is small.
include_guard(GLOBAL)
FetchContent_MakeAvailable(sqlite3mc)

add_library(micnote_sqlite STATIC "${sqlite3mc_SOURCE_DIR}/sqlite3mc_amalgamation.c")
add_library(micnote::sqlite ALIAS micnote_sqlite)
target_include_directories(micnote_sqlite SYSTEM PUBLIC "${sqlite3mc_SOURCE_DIR}")
target_compile_definitions(micnote_sqlite
  PUBLIC SQLITE_HAS_CODEC=1
  PRIVATE
    CODEC_TYPE=CODEC_TYPE_SQLCIPHER
    SQLITE_THREADSAFE=1
    SQLITE_ENABLE_FTS5=1
    SQLITE_DQS=0
    SQLITE_OMIT_LOAD_EXTENSION=1
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_TEMP_STORE=2
    SQLITE3MC_OMIT_AES_HARDWARE_SUPPORT=1)
set_target_properties(micnote_sqlite PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 11)
if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(micnote_sqlite PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  # libm for FTS5's bm25 and SQLite's math.
  find_library(MICNOTE_LIBM m)
  if(MICNOTE_LIBM)
    target_link_libraries(micnote_sqlite PRIVATE ${MICNOTE_LIBM})
  endif()
endif()
