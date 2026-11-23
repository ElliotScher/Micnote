// TF-PSA-Crypto's mutexes and condition variables on Windows
// (MBEDTLS_THREADING_ALT, see core/cmake/tfpsa/micnote_crypto_config.h).
// POSIX builds use the library's own pthread layer instead.
#if defined(_WIN32)

#include "crypto/threading_win32.hpp"

#include <mbedtls/threading.h>
#include <windows.h>

namespace micnote::crypto {

namespace {

static_assert(sizeof(mbedtls_platform_mutex_t) == sizeof(SRWLOCK));
static_assert(sizeof(mbedtls_platform_condition_variable_t) == sizeof(CONDITION_VARIABLE));

PSRWLOCK Lock(mbedtls_platform_mutex_t *m) { return reinterpret_cast<PSRWLOCK>(&m->srwlock); }
PCONDITION_VARIABLE Cv(mbedtls_platform_condition_variable_t *c) {
  return reinterpret_cast<PCONDITION_VARIABLE>(&c->cv);
}

int MutexInit(mbedtls_platform_mutex_t *m) {
  InitializeSRWLock(Lock(m));
  return 0;
}
void MutexDestroy(mbedtls_platform_mutex_t * /*m*/) {}  // nothing to free
int MutexLock(mbedtls_platform_mutex_t *m) {
  AcquireSRWLockExclusive(Lock(m));
  return 0;
}
int MutexUnlock(mbedtls_platform_mutex_t *m) {
  ReleaseSRWLockExclusive(Lock(m));
  return 0;
}
int CondInit(mbedtls_platform_condition_variable_t *c) {
  InitializeConditionVariable(Cv(c));
  return 0;
}
void CondDestroy(mbedtls_platform_condition_variable_t * /*c*/) {}  // nothing to free
int CondSignal(mbedtls_platform_condition_variable_t *c) {
  WakeConditionVariable(Cv(c));
  return 0;
}
int CondBroadcast(mbedtls_platform_condition_variable_t *c) {
  WakeAllConditionVariable(Cv(c));
  return 0;
}
int CondWait(mbedtls_platform_condition_variable_t *c, mbedtls_platform_mutex_t *m) {
  return SleepConditionVariableSRW(Cv(c), Lock(m), INFINITE, 0) != 0
             ? 0
             : MBEDTLS_ERR_THREADING_USAGE_ERROR;
}

}  // namespace

void InstallThreading() {
  mbedtls_threading_set_alt(MutexInit, MutexDestroy, MutexLock, MutexUnlock, CondInit, CondDestroy,
                            CondSignal, CondBroadcast, CondWait);
}

}  // namespace micnote::crypto

#endif
