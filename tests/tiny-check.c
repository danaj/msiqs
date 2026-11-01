#include "../embedded/tinysiqs128.h"
#include <stdio.h>
#if HAVE_UINT128
int main(void) {
  uint128_t cases[] = {UINT64_C(1000000016000000063),
    (((uint128_t)1<<61)-1)*(((uint128_t)1<<31)-1),
    (((uint128_t)1<<61)-1)*(((uint128_t)1<<61)-1)};
  unsigned i;
  if (tinysiqs128(0)!=0 || tinysiqs128(1)!=1 || tinysiqs128(2)!=2 ||
      tinysiqs128(((uint128_t)1<<127)-1)!=(((uint128_t)1<<127)-1)) return 1;
  for (i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
    uint128_t f=tinysiqs128(cases[i]);
    if (f<=1 || f>=cases[i] || cases[i]%f) return 2;
    f=tinysiqs128_composite(cases[i]);
    if (f<=1 || f>=cases[i] || cases[i]%f) return 3;
  }
  puts("PASS TinySIQS128: native-only linking, primes and verified splits");return 0;
}
#else
int main(void) { puts("SKIP TinySIQS128: no native uint128 type");return 0; }
#endif
