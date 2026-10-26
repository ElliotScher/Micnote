/*
 * libmicnote C ABI: the only interface the Flutter app (dart:ffi) and other
 * hosts use. Plain C, opaque handles and UTF-8 JSON payloads, so it stays
 * stable across compilers. app/lib/ffi/bindings.dart is generated from this
 * file by ffigen.
 */
#ifndef MICNOTE_H
#define MICNOTE_H

#if defined(MICNOTE_STATIC)
#define MN_API
#elif defined(_WIN32)
#if defined(MICNOTE_BUILDING)
#define MN_API __declspec(dllexport)
#else
#define MN_API __declspec(dllimport)
#endif
#else
#define MN_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * libmicnote's version as "MAJOR.MINOR.PATCH". The string has static storage
 * duration: never free it.
 */
MN_API const char *mn_version(void);

#ifdef __cplusplus
}
#endif

#endif /* MICNOTE_H */
