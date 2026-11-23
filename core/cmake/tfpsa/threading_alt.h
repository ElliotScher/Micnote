/*
 * MBEDTLS_THREADING_ALT's types on Windows (see micnote_crypto_config.h).
 * An SRWLOCK and a CONDITION_VARIABLE are each one pointer, initialised to
 * zero; these structs have the same layout, so this header (which every
 * TF-PSA-Crypto user includes) doesn't need <windows.h>.
 * core/src/crypto/threading_win32.cpp checks the sizes.
 */
#ifndef MICNOTE_THREADING_ALT_H
#define MICNOTE_THREADING_ALT_H

typedef struct mbedtls_platform_mutex_t {
  void *srwlock;
} mbedtls_platform_mutex_t;

typedef struct mbedtls_platform_condition_variable_t {
  void *cv;
} mbedtls_platform_condition_variable_t;

#endif
