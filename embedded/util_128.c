/* Native 128-bit helpers and BPSW primality for TinySIQS128.
 * Copyright (c) 2026 Dana Jacobsen. MIT license; see LICENSE.
 * Initially extracted from Math-Prime-Util factor128.c at fddefb08fcce91f8.
 * Included by tinysiqs128.c; do not compile separately. No GMP dependency.
 */
#ifndef MSIQS_UTIL_128_C
#define MSIQS_UTIL_128_C
#include "ptypes.h"
#include <math.h>
#include <stdbool.h>
#if HAVE_UINT128
#ifndef M_LN2
#define M_LN2 0.69314718055994530942
#endif
typedef struct {
  uint128_t n;
  uint128_t ninv;
  uint128_t r2;
  uint128_t one;
} ts_mont_t;
typedef struct { uint128_t n, ninv, r2; } mont128_t;
#define u128_mont_sqrmod128(a,ctx) u128_mont_mulmod128((a),(a),(ctx))
#define u128_is_perfect_square128(n) u128_is_perfect_square128_ret((n),NULL)
static INLINE uint128_t u128_addmod128(uint128_t a, uint128_t b, uint128_t n);
static INLINE uint128_t u128_submod128(uint128_t a, uint128_t b, uint128_t n);
static INLINE uint128_t u128_half_mod128(uint128_t a, uint128_t n);
static uint128_t u128_pow2mod128(unsigned int e, uint128_t n);
static NOINLINE uint128_t u128_gcd128(uint128_t a, uint128_t b);
static INLINE uint128_t u128_lo128(uint128_t a, uint128_t b);
static INLINE void u128_mul256(uint128_t a, uint128_t b,
                          uint128_t *lo_out, uint128_t *hi_out);
static INLINE uint128_t u128_mulhi128(uint128_t a, uint128_t b);
static INLINE uint128_t u128_mont_mulmod128(uint128_t a, uint128_t b,
                                       const mont128_t *ctx);
static INLINE uint128_t u128_mont_enter128(uint128_t a, const mont128_t *ctx);
static void u128_mont_setup128(mont128_t *ctx, uint128_t n);
static uint128_t u128_mont_powmod128_u128(uint128_t a, uint128_t k,
                                     const mont128_t *ctx);
static bool u128_miller_rabin128(uint128_t n, uint128_t base);
static int u128_kronecker128(uint128_t a, int asign, uint128_t b, int bsign);
static int u128_jacobi128(int128_t a_in, uint128_t n);
static bool u128_select_extra_strong_parameters128(uint32_t *Pout, uint128_t n);
static bool u128_is_extra_strong_lucas128(uint128_t n);
static bool u128_is_bpsw128(uint128_t n);
static uint64_t u128_isqrt128(uint128_t n);
static bool u128_is_perfect_square128_ret(uint128_t n, uint64_t *root);

static INLINE uint128_t u128_addmod128(uint128_t a, uint128_t b, uint128_t n) {
  /* Avoid relying on unsigned 128-bit overflow. */
  uint128_t nb = n - b;
  return (a >= nb) ? a - nb : a + b;
}

static INLINE uint128_t u128_submod128(uint128_t a, uint128_t b, uint128_t n) {
  return (a >= b) ? a - b : n - (b - a);
}

static INLINE uint128_t u128_half_mod128(uint128_t a, uint128_t n) {
  return (a & 1)  ?  (a >> 1) + (n >> 1) + 1  :  a >> 1;
}

static uint128_t u128_pow2mod128(unsigned int e, uint128_t n) {
  uint128_t r = 1;
  while (e-- > 0)
    r = u128_addmod128(r, r, n);
  return r;
}

/* Keep NOINLINE: GCC 14 has had _BitInt(128) inlining problems here. */

static NOINLINE uint128_t u128_gcd128(uint128_t a, uint128_t b) {
  while (b) { uint128_t t = b; b = a % b; a = t; }
  return a;
}

static INLINE uint128_t u128_lo128(uint128_t a, uint128_t b) {
  uint64_t a0 = (uint64_t)a,  a1 = (uint64_t)(a >> 64);
  uint64_t b0 = (uint64_t)b,  b1 = (uint64_t)(b >> 64);
  uint128_t lo = (uint128_t)a0 * b0;
  lo += (uint128_t)a0 * b1 << 64;
  lo += (uint128_t)a1 * b0 << 64;
  return lo;
}

static INLINE void u128_mul256(uint128_t a, uint128_t b,
                          uint128_t *lo_out, uint128_t *hi_out) {
  uint64_t a0 = (uint64_t)a,  a1 = (uint64_t)(a >> 64);
  uint64_t b0 = (uint64_t)b,  b1 = (uint64_t)(b >> 64);

  uint128_t p00 = (uint128_t)a0 * b0;
  uint128_t p01 = (uint128_t)a0 * b1;
  uint128_t p10 = (uint128_t)a1 * b0;
  uint128_t p11 = (uint128_t)a1 * b1;

  /* lo = p00 + ((p01 + p10) << 64), tracked with carry into hi */
  uint128_t mid  = p01 + p10;
  int       mc   = (mid < p01);          /* carry from mid addition */
  uint128_t lo   = p00 + (mid << 64);
  int       lc   = (lo < p00);           /* carry from lo addition */

  *lo_out = lo;
  *hi_out = p11 + (mid >> 64) + ((uint128_t)mc << 64) + lc;
}

static INLINE uint128_t u128_mulhi128(uint128_t a, uint128_t b) {
  uint64_t a0 = (uint64_t)a,  a1 = (uint64_t)(a >> 64);
  uint64_t b0 = (uint64_t)b,  b1 = (uint64_t)(b >> 64);

  uint128_t p00 = (uint128_t)a0 * b0;
  uint128_t p01 = (uint128_t)a0 * b1;
  uint128_t p10 = (uint128_t)a1 * b0;
  uint128_t p11 = (uint128_t)a1 * b1;

  uint128_t mid = p01 + p10;
  int       mc  = (mid < p01);
  uint128_t lo  = p00 + (mid << 64);     /* only needed for carry lc */
  int       lc  = (lo < p00);

  return p11 + (mid >> 64) + ((uint128_t)mc << 64) + lc;
}

static INLINE uint128_t u128_mont_mulmod128(uint128_t a, uint128_t b,
                                       const mont128_t *ctx) {
  uint128_t lo, hi;
  u128_mul256(a, b, &lo, &hi);

  /* m = lo * ninv  (only low 128 bits needed) */
  uint128_t m = u128_lo128(lo, ctx->ninv);

  /* m·n ≡ -lo (mod R), so lo + mn_lo = R when lo≠0, 0 otherwise.
   * carry into the high half is simply (lo != 0); no need to compute mn_lo. */
  int carry = (lo != 0);
  uint128_t mn_hi = u128_mulhi128(m, ctx->n);

  /* hi + mn_hi + carry may exceed 2^128 when n > 2^127 (true result still
   * in [0, 2n), so one conditional subtract suffices).  Track overflow bit. */
  uint128_t t = hi + mn_hi;
  int ov = (t < hi);
  t += (uint128_t)carry;
  ov += (t < (uint128_t)carry);

  /* When ov=1 the true value is t + 2^128 >= n; uint128_t wraparound in
   * t - ctx->n correctly gives (t + 2^128 - n) mod 2^128. */
  if (ov || t >= ctx->n) return t - ctx->n;
  return t;
}

static INLINE uint128_t u128_mont_enter128(uint128_t a, const mont128_t *ctx) {
  return u128_mont_mulmod128(a % ctx->n, ctx->r2, ctx);
}

static void u128_mont_setup128(mont128_t *ctx, uint128_t n) {
  ctx->n = n;

  /* ninv = n^{-1} mod 2^128 via Hensel lifting.
   * Each step doubles the number of correct bits: x = x*(2 - n*x). */
  uint128_t x = (3*n)^2;                 /*   4 bits (n odd) */
  x *= 2 - n * x;                        /*   8 bits */
  x *= 2 - n * x;                        /*  16 bits */
  x *= 2 - n * x;                        /*  32 bits */
  x *= 2 - n * x;                        /*  64 bits */
  x *= 2 - n * x;                        /* 128 bits */
  /* REDC add-form needs m = T*(-n^{-1}) mod R; store the negation */
  ctx->ninv = -x;

  /* r2 = R^2 mod n = (2^128)^2 mod n = 2^256 mod n. */
  ctx->r2 = u128_pow2mod128(256,n);
}

static uint128_t u128_mont_powmod128_u128(uint128_t a, uint128_t k,
                                     const mont128_t *ctx) {
  uint128_t r = u128_mont_enter128(1, ctx);
  while (k > 0) {
    if (k & 1) r = u128_mont_mulmod128(r, a, ctx);
    k >>= 1;
    if (k) a = u128_mont_sqrmod128(a, ctx);
  }
  return r;
}

static bool u128_miller_rabin128(uint128_t n, uint128_t base) {
  uint128_t d = n - 1;
  mont128_t ctx;
  int s = 0;

  while (!(d & 1)) { d >>= 1; s++; }

  u128_mont_setup128(&ctx, n);
  uint128_t mont1 = u128_mont_enter128(1, &ctx);
  uint128_t montbase = (base == 2) ? u128_addmod128(mont1, mont1, n)
                                   : u128_mont_enter128(base % n, &ctx);
  uint128_t x = u128_mont_powmod128_u128(montbase, d, &ctx);
  uint128_t montm1 = n - mont1;
  if (x == mont1 || x == montm1) return 1;
  while (--s > 0) {
    x = u128_mont_sqrmod128(x, &ctx);
    if (x == montm1) return 1;
  }
  return 0;
}

static int u128_kronecker128(uint128_t a, int asign, uint128_t b, int bsign)
{
  int s = 1;

  if (b == 0) return a == 1;

  /* (a/-b) differs from (a/b) only when a is negative. */
  if (asign < 0 && bsign < 0) s = -s;

  if (!(b & 1)) {
    int odd_power = 0;
    if (!(a & 1)) return 0;
    do {
      b >>= 1;
      odd_power = !odd_power;
    } while (!(b & 1));
    if (odd_power && ((a & 7) == 3 || (a & 7) == 5)) s = -s;
  }

  /* The denominator is now positive and odd. */
  if (asign < 0 && (b & 3) == 3) s = -s;

  while (a != 0) {
    int odd_power = 0;
    while (!(a & 1)) {
      a >>= 1;
      odd_power = !odd_power;
    }
    if (odd_power && ((b & 7) == 3 || (b & 7) == 5)) s = -s;
    if ((a & 3) == 3 && (b & 3) == 3) s = -s;
    {
      uint128_t rem = b % a;
      b = a;
      a = rem;
    }
  }
  return (b == 1) ? s : 0;
}

static int u128_jacobi128(int128_t a_in, uint128_t n)
{
  uint128_t a;
  int asign = 1;

  if (a_in < 0) {
    /* This form is safe even if a_in is the minimum signed value. */
    a = (uint128_t)(-(a_in + 1)) + 1;
    asign = -1;
  } else {
    a = (uint128_t)a_in;
  }
  return u128_kronecker128(a, asign, n, 1);
}

static bool u128_select_extra_strong_parameters128(uint32_t *Pout, uint128_t n) {
  uint128_t g;
  uint64_t D;
  uint32_t P = 3;
  int j;

  while (1) {
    D = (uint64_t)P * P - 4;
    j = u128_jacobi128((int128_t)D, n);
    if (j == 0) {
      g = u128_gcd128((uint128_t)D, n);
      if (g != 1 && g != n) return 0;
    }
    if (j == -1) break;
    if (P == 23 && u128_is_perfect_square128(n)) return 0;
    if (++P > 65535)
      croak("lucas_extrastrong_params: P exceeded 65535");
  }
  *Pout = P;
  return 1;
}

static bool u128_is_extra_strong_lucas128(uint128_t n) {
  mont128_t ctx;
  uint128_t U, V, d, bit, t2, unext, vnext;
  uint128_t mont1, mont2, montP, montD;
  uint64_t D;
  uint32_t P;
  int s = 0;

  if (!u128_select_extra_strong_parameters128(&P, n)) return 0;
  D = (uint64_t)P * P - 4;

  d = n + 1;
  while (!(d & 1)) { d >>= 1; s++; }

  u128_mont_setup128(&ctx, n);
  mont1 = u128_mont_enter128(1, &ctx);
  mont2 = u128_addmod128(mont1, mont1, n);
  montP = u128_mont_enter128(P, &ctx);
  montD = u128_mont_enter128(D, &ctx);
  U = mont1;
  V = montP;

  bit = (uint128_t)1 << 126;
  while (bit > d) bit >>= 1;
  bit >>= 1;
  while (bit > 0) {
    U = u128_mont_mulmod128(U, V, &ctx);
    V = u128_submod128(u128_mont_sqrmod128(V, &ctx), mont2, n);
    if (d & bit) {
      t2 = u128_mont_mulmod128(U, montD, &ctx);
      unext = u128_addmod128(u128_mont_mulmod128(U, montP, &ctx), V, n);
      vnext = u128_addmod128(u128_mont_mulmod128(V, montP, &ctx), t2, n);
      U = u128_half_mod128(unext, n);
      V = u128_half_mod128(vnext, n);
    }
    bit >>= 1;
  }

  if (U == 0 && (V == mont2 || V == n-mont2)) return 1;
  s--;
  while (s--) {
    if (V == 0) return 1;
    if (s) V = u128_submod128(u128_mont_sqrmod128(V, &ctx), mont2, n);
  }
  return 0;
}

static bool u128_is_bpsw128(uint128_t n) {
  if (n < 7) return (n==2 || n==3 || n==5);
  if (!(n & 1) || n == (uint128_t)-1) return 0;
  if (!u128_miller_rabin128(n, 2)) return 0;
  if (!u128_is_extra_strong_lucas128(n)) return 0;
  return 1;
}

static uint64_t u128_isqrt128(uint128_t n) {
  if (n == 0) return 0;
  /* Initial estimate: r = 2^ceil(bits/2), an overestimate of sqrt(n) */
  uint128_t r = 1, tmp = n;
  while (tmp > 3) { tmp >>= 2; r <<= 1; }
  r <<= 1;   /* ensure we start from above so Newton descends to floor */
  /* Newton's method descends from above: r = (r + n/r) / 2 */
  while (1) {
    uint128_t rn = (r + n / r) >> 1;
    if (rn >= r) break;
    r = rn;
  }
  /* r may still be one too high; adjust down */
  while (r * r > n) r--;
  return (uint64_t)r;
}

static bool u128_is_perfect_square128_ret(uint128_t n, uint64_t *root) {
  uint64_t r;
  uint32_t m;

  /* Fast filters reject 95.0% of non-squares. */
  if ((UINT64_C(1) << ((uint64_t)n & 63)) &
      UINT64_C(0xfdfdfdedfdfcfdec))
    return 0;
  m = (uint32_t)(n % 45);
  if ((UINT64_C(1) << m) & UINT64_C(0xfffffeeb7df6f9ec))
    return 0;

  r = u128_isqrt128(n);
  if (root != 0) *root = r;
  return (uint128_t)r * r == n;
}

static uint32_t ts_bits128(uint128_t n) {
  uint64_t hi = (uint64_t)(n >> 64);
  uint64_t lo = (uint64_t)n;
  uint32_t bits = 0;
  uint64_t v = hi ? hi : lo;
  if (v == 0)
    return 0;
#if defined(__GNUC__) || defined(__clang__)
  bits = 64U - (uint32_t)__builtin_clzll(v);
#else
  while (v != 0) { bits++; v >>= 1; }
#endif
  return hi ? bits + 64U : bits;
}

static double ts_log128(uint128_t n) {
  uint64_t hi = (uint64_t)(n >> 64);
  uint64_t lo = (uint64_t)n;
  if (hi == 0)
    return log((double)lo);
  return log((double)hi + ldexp((double)lo, -64)) + 64.0 * M_LN2;
}

/* Avoid compiler runtime helpers for uint128_t-to-double conversion. */
static double ts_get_d(uint128_t n) {
  double d = (double)(uint32_t)(n >> 96);
  d = d * 4294967296.0 + (double)(uint32_t)(n >> 64);
  d = d * 4294967296.0 + (double)(uint32_t)(n >> 32);
  return d * 4294967296.0 + (double)(uint32_t)n;
}

static uint128_t ts_abs_i128(int128_t value) {
  uint128_t u = (uint128_t)value;
  return value < 0 ? (uint128_t)0 - u : u;
}

static uint32_t ts_signed_mod_u32(int128_t value, uint32_t p) {
  uint32_t r = (uint32_t)(ts_abs_i128(value) % p);
  return value < 0 && r != 0 ? p - r : r;
}

static uint128_t ts_signed_mod_n(int128_t value, uint128_t n) {
  uint128_t r = ts_abs_i128(value) % n;
  return value < 0 && r != 0 ? n - r : r;
}

static uint32_t ts_ctz32(uint32_t n) {
  if (n == 0)
    croak("tinysiqs128: ctz of zero");
#if defined(__GNUC__) || defined(__clang__)
  return (uint32_t)__builtin_ctz(n);
#else
  uint32_t count = 0;
  while (!(n & 1U)) { n >>= 1; count++; }
  return count;
#endif
}

static unsigned int ts_ctz64(uint64_t n) {
  if (n == 0)
    croak("tinysiqs128: ctz of zero");
#if defined(__GNUC__) || defined(__clang__)
  return (unsigned int)__builtin_ctzll(n);
#else
  unsigned int count = 0;
  while (!(n & 1U)) { n >>= 1; count++; }
  return count;
#endif
}

static uint64_t ts_isqrt128(uint128_t n) {
  uint128_t r, next;
  uint32_t bits;
  if (n == 0)
    return 0;
  bits = ts_bits128(n);
  r = (uint128_t)1 << ((bits + 1U) / 2U);
  for (;;) {
    next = (r + n / r) >> 1;
    if (next >= r)
      break;
    r = next;
  }
  while (r > n / r)
    r--;
  return (uint64_t)r;
}

static uint128_t ts_gcd128(uint128_t a, uint128_t b) {
  while (b != 0) {
    uint128_t t = a % b;
    a = b;
    b = t;
  }
  return a;
}

static uint128_t ts_addmod128(uint128_t a, uint128_t b, uint128_t n) {
  uint128_t nb = n - b;
  return a >= nb ? a - nb : a + b;
}

static uint128_t ts_submod128(uint128_t a, uint128_t b, uint128_t n) {
  return a >= b ? a - b : n - (b - a);
}

static uint128_t ts_pow2mod128(unsigned int exponent, uint128_t n) {
  uint128_t r = 1;
  while (exponent-- != 0)
    r = ts_addmod128(r, r, n);
  return r;
}

/* Low 128 bits of a full product. */
static uint128_t ts_lo128(uint128_t a, uint128_t b) {
  uint64_t a0 = (uint64_t)a, a1 = (uint64_t)(a >> 64);
  uint64_t b0 = (uint64_t)b, b1 = (uint64_t)(b >> 64);
  uint128_t lo = (uint128_t)a0 * b0;
  lo += ((uint128_t)a0 * b1) << 64;
  lo += ((uint128_t)a1 * b0) << 64;
  return lo;
}

static void ts_mul256(uint128_t a, uint128_t b,
                      uint128_t *lo_out, uint128_t *hi_out) {
  uint64_t a0 = (uint64_t)a, a1 = (uint64_t)(a >> 64);
  uint64_t b0 = (uint64_t)b, b1 = (uint64_t)(b >> 64);
  uint128_t p00 = (uint128_t)a0 * b0;
  uint128_t p01 = (uint128_t)a0 * b1;
  uint128_t p10 = (uint128_t)a1 * b0;
  uint128_t p11 = (uint128_t)a1 * b1;
  uint128_t mid = p01 + p10;
  int middle_carry = mid < p01;
  uint128_t lo = p00 + (mid << 64);
  int low_carry = lo < p00;
  *lo_out = lo;
  *hi_out = p11 + (mid >> 64)
          + ((uint128_t)middle_carry << 64) + (uint128_t)low_carry;
}

static uint128_t ts_mulhi128(uint128_t a, uint128_t b) {
  uint128_t lo, hi;
  ts_mul256(a, b, &lo, &hi);
  (void)lo;
  return hi;
}

static uint128_t ts_mont_mul(uint128_t a, uint128_t b,
                             const ts_mont_t *ctx) {
  uint128_t lo, hi, m, mn_hi, t;
  int carry, overflow;
  ts_mul256(a, b, &lo, &hi);
  m = ts_lo128(lo, ctx->ninv);
  carry = lo != 0;
  mn_hi = ts_mulhi128(m, ctx->n);
  t = hi + mn_hi;
  overflow = t < hi;
  {
    uint128_t old = t;
    t += (uint128_t)carry;
    overflow += t < old;
  }
  return overflow || t >= ctx->n ? t - ctx->n : t;
}

static uint128_t ts_mont_enter(uint128_t a, const ts_mont_t *ctx) {
  return ts_mont_mul(a % ctx->n, ctx->r2, ctx);
}

static uint128_t ts_mont_exit(uint128_t a, const ts_mont_t *ctx) {
  return ts_mont_mul(a, 1, ctx);
}

static void ts_mont_setup(ts_mont_t *ctx, uint128_t n) {
  uint128_t x;
  if (n <= 1 || !(n & 1U))
    croak("tinysiqs128: invalid Montgomery modulus");
  ctx->n = n;
  x = (3 * n) ^ 2;
  x *= 2 - n * x;
  x *= 2 - n * x;
  x *= 2 - n * x;
  x *= 2 - n * x;
  x *= 2 - n * x;
  ctx->ninv = (uint128_t)0 - x;
  ctx->r2 = ts_pow2mod128(256, n);
  ctx->one = ts_mont_enter(1, ctx);
}

static uint128_t ts_mont_pow_u32(uint128_t a, uint32_t exponent,
                                 const ts_mont_t *ctx) {
  uint128_t r = ctx->one;
  while (exponent != 0) {
    if (exponent & 1U)
      r = ts_mont_mul(r, a, ctx);
    exponent >>= 1;
    if (exponent != 0)
      a = ts_mont_mul(a, a, ctx);
  }
  return r;
}

/* Coefficients are kept modulo n in Montgomery form. */
static uint128_t ts_modinv128(uint128_t a, const ts_mont_t *ctx) {
  uint128_t r0 = ctx->n, r1 = a % ctx->n;
  uint128_t t0 = 0, t1 = ctx->one;
  if (r1 == 0)
    return 0;
  while (r1 != 0) {
    uint128_t q = r0 / r1;
    uint128_t r = r0 - q * r1;
    uint128_t qm = ts_mont_enter(q, ctx);
    uint128_t t = ts_submod128(t0, ts_mont_mul(qm, t1, ctx), ctx->n);
    r0 = r1;
    r1 = r;
    t0 = t1;
    t1 = t;
  }
  return r0 == 1 ? ts_mont_exit(t0, ctx) : 0;
}

/* Compare base^exponent with limit without overflowing. */
static int ts_power_compare(uint128_t base, uint32_t exponent,
                            uint128_t limit) {
  uint128_t product = 1;
  uint32_t i;
  for (i = 0; i < exponent; i++) {
    if (base != 0 && product > limit / base)
      return 1;
    product *= base;
  }
  return product < limit ? -1 : product > limit ? 1 : 0;
}

static uint128_t ts_exact_root(uint128_t n, uint32_t exponent) {
  uint32_t bits = ts_bits128(n);
  uint32_t root_bits = (bits + exponent - 1U) / exponent;
  uint128_t lo = 2, hi;
  if (root_bits >= 128)
    hi = n;
  else
    hi = (uint128_t)1 << root_bits;
  while (lo <= hi) {
    uint128_t mid = lo + ((hi - lo) >> 1);
    int cmp = ts_power_compare(mid, exponent, n);
    if (cmp == 0)
      return mid;
    if (cmp < 0)
      lo = mid + 1;
    else {
      if (mid == 0)
        break;
      hi = mid - 1;
    }
  }
  return 0;
}

static uint128_t ts_mont_neg(uint128_t value, const ts_mont_t *mont) {
  return value == 0 ? 0 : mont->n - value;
}

#endif /* HAVE_UINT128 */
#endif /* MSIQS_UTIL_128_C */
