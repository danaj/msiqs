/* Compare the rebuilt relation graph's completed relations with live collection.
 * A cached final partition alone must not hide a faulty relation replay. */
#define main journal_frontend_main
#include "../../msiqs.c"
#undef main
#include "../../siqs.c"

int main(int argc, char **argv) {
  siqs_journal_t *journal;
  siqs_factor_array_t result;
  siqs_ctx_t ctx;
  mpz_t n;
  uint64_t hash = UINT64_C(0x876543210abcdef);
  uint32_t i, k;
  if (argc != 2) return 2;
  mpz_init_set_str(n,"57896044618658097711785492504343953899242261795684735710927136105699223797919",10);
  journal = gmp_siqs_journal_open(argv[1], n);
  siqs_factor_array_init(&result,n); result.journal = journal;
  siqs_ctx_init(&ctx,n,n,&result,NULL,0);
#ifdef PSIQS
  ctx.nthreads = journal->reading ? 1U : 4U;
#endif
  if (!siqs_run(&ctx)) return 3;
  for (i=0;i<ctx.full_count;i++) {
    siqs_full_relation_t *r=ctx.full[i];
    hash=siqs_mix64(hash ^ mpz_fdiv_ui(r->y,4294967291UL));
    hash=siqs_mix64(hash ^ mpz_fdiv_ui(r->y,4294967279UL));
    hash=siqs_mix64(hash ^ r->nfactors);
    for (k=0;k<r->nfactors;k++)
      hash=siqs_mix64(hash ^ ((uint64_t)(ctx.lp_combiner != NULL ? siqs_lp_archive_row(r,k) : r->factors[k].row)<<32) ^ (ctx.lp_combiner != NULL ? siqs_lp_archive_exponent(r,k) : r->factors[k].exponent));
  }
  printf("{\"raw\":%u,\"full\":%u,\"smooth\":%llu,\"one_lp\":%llu,\"two_lp\":%llu,"
      "\"split_squfof\":%llu,\"split_ecm\":%llu,\"digest\":\"%016llx\"}\n",
      siqs_partial_count(&ctx),siqs_relation_count(&ctx),(unsigned long long)ctx.accepted_smooth,
      (unsigned long long)ctx.accepted_one_lp,(unsigned long long)ctx.accepted_two_lp,
      (unsigned long long)ctx.split_squfof,(unsigned long long)ctx.split_ecm,
      (unsigned long long)hash);
  sj_finish_call(journal,n,result.values,result.count);
  siqs_ctx_clear(&ctx);gmp_siqs_journal_close(journal);
  gmp_siqs_free(result.values,result.count);free(result.primality);mpz_clear(n);
  return 0;
}
