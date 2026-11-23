/*
 * Micnote's additions to TF-PSA-Crypto's default config
 * (TF_PSA_CRYPTO_USER_CONFIG_FILE, set in core/CMakeLists.txt).
 *
 * Thread safety: the job queue (stage 2 §4.8) runs the sync and the
 * transcribe job on their own threads, and both decrypt recordings while the
 * host's thread may be calling in too. Without MBEDTLS_THREADING_C the PSA
 * key-slot table is unprotected (ThreadSanitizer finds the race in
 * psa_allocate_volatile_key_slot). POSIX uses pthreads; Windows uses
 * core/src/crypto/threading_win32.cpp (SRWLOCK and CONDITION_VARIABLE),
 * installed by micnote::crypto::Init() before psa_crypto_init().
 */
#define MBEDTLS_THREADING_C
#if defined(_WIN32)
#define MBEDTLS_THREADING_ALT
#else
#define MBEDTLS_THREADING_PTHREAD
#endif

/* The self-tests aren't called, and their multiplication counter in ecp.c
 * is a shared global the threads would race on. */
#undef MBEDTLS_SELF_TEST
