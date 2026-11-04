/* Fixed-width types and small portability helpers for msiqs.
 * Copyright (c) 2026 Dana Jacobsen. MIT license; see LICENSE.
 * C99 and a uint64_t type are required. Native 128-bit support is optional.
 */
#ifndef MSIQS_PTYPES_H
#define MSIQS_PTYPES_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#ifndef HAVE_UINT128
# if (defined(__SIZEOF_INT128__) && !defined(__CUDACC__)) || \
     (defined(__BITINT_MAXWIDTH__) && __BITINT_MAXWIDTH__ >= 128)
#  define HAVE_UINT128 1
# else
#  define HAVE_UINT128 0
# endif
#endif
#if HAVE_UINT128
# if defined(__SIZEOF_INT128__)
  typedef unsigned __int128 uint128_t;
  typedef signed __int128 int128_t;
# else
  typedef unsigned _BitInt(128) uint128_t;
  typedef signed _BitInt(128) int128_t;
# endif
#endif

#if defined(__GNUC__) || defined(__clang__)
# define MAYBE_UNUSED __attribute__((unused))
# define NOINLINE __attribute__((noinline))
#else
# define MAYBE_UNUSED
# define NOINLINE
#endif
#ifdef _MSC_VER
# define INLINE __inline
# define RESTRICT __restrict
#else
# define INLINE inline
# define RESTRICT restrict
#endif

/* Fatal allocation and invariant errors terminate the standalone caller. */
static MAYBE_UNUSED void msiqs_fatal(const char *format, ...) {
  const char *end = format;
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  while (*end) end++;
  if (end == format || end[-1] != '\n') fputc('\n', stderr);
  exit(3);
}
#define croak msiqs_fatal
#endif
