#include "../embedded/tinysiqs128.c"
#include <gmp.h>
#include <stdio.h>
#if HAVE_UINT128
static uint64_t rng=UINT64_C(0x8e721c06a9b5d43f);
static uint64_t next64(void) { rng^=rng<<13;rng^=rng>>7;rng^=rng<<17;return rng; }
static int check(mpz_t z,uint128_t n) {
  uint64_t words[2]={(uint64_t)n,(uint64_t)(n>>64)};
  mpz_import(z,2,-1,sizeof(uint64_t),0,0,words);
  return (u128_is_bpsw128(n)!=0)==(mpz_probab_prime_p(z,25)!=0);
}
int main(void) {
  unsigned i;mpz_t z;mpz_init(z);
  for(i=0;i<100000;i++) if(!check(z,i))return 1;
  for(i=0;i<5000;i++) {
    uint128_t n=((uint128_t)next64()<<64)|next64();
    unsigned bits=1+i%128;if(bits<128)n&=(((uint128_t)1<<bits)-1);
    if(!check(z,n))return 2;
    { uint64_t r=next64();uint128_t square=(uint128_t)r*r;
      if(!u128_is_perfect_square128(square)||u128_isqrt128(square)!=r)return 3;
      if(!check(z,square))return 4;
    }
  }
  for(i=0;i<128;i++) {uint128_t n=(uint128_t)1<<i;
    if(!check(z,n)||!check(z,n-1)||!check(z,n+1))return 5;}
  { uint64_t ps[]={2047,3277,4033,4681,8321,15841,29341,42799,
      UINT64_C(341550071728321),UINT64_C(3825123056546413051)};
    for(i=0;i<sizeof(ps)/sizeof(ps[0]);i++)if(!check(z,ps[i]))return 6;}
  for(i=0;i<512;i++)if(!check(z,(uint128_t)-1-i))return 7;
  mpz_clear(z);puts("PASS native BPSW: small exhaustive, random, squares, pseudoprimes and uint128 boundaries");return 0;
}
#else
int main(void) { puts("SKIP native BPSW: no uint128 type");return 0; }
#endif
