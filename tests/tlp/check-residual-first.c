/* Independent trial-division oracle plus exact worker-stop/lifetime cases. */
#define SIQS_RF_TEST_HOOKS 1
#define main existing_private_checks
#include "check.c"
#undef main

#if !SIQS_RESIDUAL_FIRST
# error "Compile this check with SIQS_RESIDUAL_FIRST=1"
#endif

static void rf_test_context_threads(siqs_ctx_t *ctx, siqs_factor_array_t *result,
                             const mpz_t n, int enabled, unsigned threads) {
  uint32_t i;
  test_context(ctx, result, n);
  ctx->params.sieve_start = 2; ctx->params.smooth_bound = 500;
  ctx->params.large_prime_bound = 50; ctx->params.tlp_smooth_bound = 6050;
  mpz_set_ui(ctx->tlp_upper, 6050);
  siqs_tlp_policy_clear(ctx); siqs_tlp_policy_init(ctx);
  ctx->prime = siqs_malloc(5*sizeof(*ctx->prime));
  for (i = 0; i < 5; ++i) ctx->prime[i] = ctx->fb[i].p;
  if (enabled) {
    const struct siqs_tlp_state *resolver;
    siqs_tlp_clear(ctx);
    ctx->residual_first = 1;
    ctx->nthreads = threads;
    siqs_tlp_init(ctx);
    siqs_rf_init(ctx);
    assert(ctx->tlp->capacity == 0 && ctx->tlp->pending == NULL &&
           ctx->tlp->inputs == NULL && ctx->tlp->gcds == NULL);
    resolver = ctx->rf->slots[0]->resolver.tlp;
    assert(resolver->capacity == ctx->rf->lp_capacity && resolver->pending == NULL &&
           resolver->inputs != NULL && resolver->gcds != NULL);
  }
}

static void rf_test_context(siqs_ctx_t *ctx, siqs_factor_array_t *result,
                             const mpz_t n, int enabled) {
  rf_test_context_threads(ctx,result,n,enabled,4);
}

static void rf_signature(siqs_ctx_t *ctx, const siqs_full_relation_t *r, mpz_t out) {
  mpz_t p; uint32_t i;
  mpz_init(p); mpz_set_ui(out, 1);
  for (i = 0; i < r->nfactors; ++i) {
    uint32_t row = siqs_lp_archive_row(r, i);
    uint32_t exponent = siqs_lp_archive_exponent(r, i);
    if (row == 0) { if (exponent & 1U) mpz_neg(out, out); }
    else {
      siqs_mpz_set_u64(p, siqs_matrix_prime(ctx, row));
      mpz_pow_ui(p, p, exponent); mpz_mul(out, out, p);
    }
  }
  mpz_clear(p);
}

static void rf_equal(siqs_ctx_t *a, siqs_ctx_t *b) {
  uint32_t i,j;mpz_t left,right;
  assert(a->full_count==b->full_count);
  assert(a->accepted_smooth==b->accepted_smooth);
  assert(a->accepted_one_lp==b->accepted_one_lp);
  assert(a->accepted_two_lp==b->accepted_two_lp);
  assert(a->accepted_three_lp==b->accepted_three_lp);
  assert(siqs_relation_count(a)==siqs_relation_count(b));
  mpz_inits(left,right,NULL);
  for(i=0;i<a->full_count;++i) {
    siqs_verify_full_relation(a,a->full[i]);rf_signature(a,a->full[i],left);
    for(j=0;j<b->full_count;++j)if(mpz_cmp(a->full[i]->y,b->full[j]->y)==0) {
      rf_signature(b,b->full[j],right);if(mpz_cmp(left,right)==0)break;
    }
    assert(j<b->full_count);
  }
  mpz_clears(left,right,NULL);
}

static void rf_case(unsigned index,mpz_t norm,mpz_t y,
                     siqs_factor_t known[4],uint32_t *nknown) {
  unsigned kind=index%9U,k=(index/9U)%31U;
  unsigned long residual=kind==0?1:kind==1?13:kind==2?169:
                         kind==3?4199:kind==4?2873:kind==5?1009:
                         kind==6?1201:kind==7?4199:1000003;
  unsigned long root=kind==0?1:kind==1?9125:kind==2?13:
                     kind==3?991:kind==4?8720:kind==7?7713:1;
  uint32_t used=0;
  if(kind==7){known[used].row=0;known[used++].exponent=1;}
  known[used].row=1;known[used++].exponent=kind==7?1:2;
  known[used].row=2;known[used++].exponent=kind==7?1:2;
  known[used].row=4;known[used++].exponent=2;
  *nknown=used;
  mpz_ui_pow_ui(norm,7,2U*k);mpz_mul_ui(norm,norm,residual);
  mpz_ui_pow_ui(y,7,k+1U);mpz_mul_ui(y,y,root);
  if(kind!=7)mpz_mul_ui(y,y,6);
  if(kind==4){mpz_mul_ui(norm,norm,625);mpz_mul_ui(y,y,25);}
  mpz_mod_ui(y,y,10403);
}

static void rf_trial_oracle(siqs_ctx_t *ctx,const mpz_t norm,const mpz_t y,
                             const siqs_factor_t *known,uint32_t nknown) {
  siqs_factor_t factors[64];uint32_t i,count=nknown;mpz_t rest;
  memcpy(factors,known,nknown*sizeof(*factors));mpz_init_set(rest,norm);
  for(i=ctx->params.sieve_start;i<ctx->params.fb_size;++i)
    if(mpz_divisible_ui_p(rest,ctx->prime[i])) {
      factors[count].row=i+1U;
      factors[count++].exponent=(uint32_t)siqs_remove_ui(rest,ctx->prime[i]);
    }
  count=siqs_rf_coalesce(factors,count);
  siqs_resolve_cofactor_3lp(ctx,rest,y,factors,count);mpz_clear(rest);
}

static void rf_batch_sizes(void) {
  const unsigned sizes[]={0,1,31,32,33,67,2050};size_t which;
  for(which=0;which<sizeof(sizes)/sizeof(*sizes);++which) {
    siqs_ctx_t normal,rf;siqs_factor_array_t nr,rr;
    mpz_t n,norm,y;unsigned i;
    mpz_init_set_ui(n,10403);mpz_inits(norm,y,NULL);
    rf_test_context(&normal,&nr,n,0);rf_test_context(&rf,&rr,n,1);
    for(i=0;i<sizes[which];++i) {
      siqs_factor_t known[4];uint32_t count;siqs_raw_relation_t *packet;
      rf_case(i,norm,y,known,&count);
      rf_trial_oracle(&normal,norm,y,known,count);
      packet=siqs_rf_packet_new(&rf.rf->pending_arena,y,known,count,norm,NULL);
      siqs_rf_queue(&rf,packet);
    }
    siqs_tlp_flush(&normal);siqs_rf_flush(&rf);
    assert(!siqs_tlp_pending_count(&rf));assert(rf.rf->count==0);
    for(i=0;i<rf.rf->job_count;++i)assert(!rf.rf->slots[i]->running&&
        rf.rf->slots[i]->flight_count==0&&rf.rf->slots[i]->lp_count==0);
    rf_equal(&normal,&rf);
    release_context(&normal,&nr);release_context(&rf,&rr);
    mpz_clears(n,norm,y,NULL);
  }
}

static void rf_product_store(void) {
  siqs_tlp_prime_store *store=NULL;siqs_tlp_batch *batch=NULL;
  mpz_t product,input[5],gcds[5];size_t i;
  const unsigned long values[]={55,77,25,17,2},want[]={55,11,5,1,1};
  mpz_init_set_ui(product,55);
  assert(siqs_tlp_prime_store_from_product(&store,product,5,11,2)==SIQS_TLP_OK);
  assert(mpz_cmp_ui(product,55)==0);
  assert(store->stats.prime_count==2&&store->stats.block_count==1&&
         store->stats.first_prime==5&&store->stats.last_prime==11);
  assert(siqs_tlp_batch_create(&batch,store,8)==SIQS_TLP_OK);
  for(i=0;i<5;++i){mpz_init_set_ui(input[i],values[i]);mpz_init(gcds[i]);}
  assert(siqs_tlp_batch_gcd(batch,input,gcds,5)==SIQS_TLP_OK);
  for(i=0;i<5;++i)assert(mpz_cmp_ui(gcds[i],want[i])==0);
  siqs_tlp_batch_destroy(batch);siqs_tlp_prime_store_destroy(store);
  mpz_set_ui(product,1);
  assert(siqs_tlp_prime_store_from_product(&store,product,0,0,0)==SIQS_TLP_OK);
  assert(siqs_tlp_batch_create(&batch,store,8)==SIQS_TLP_OK);
  assert(siqs_tlp_batch_gcd(batch,input,gcds,5)==SIQS_TLP_OK);
  for(i=0;i<5;++i)assert(mpz_cmp_ui(gcds[i],1)==0);
  siqs_tlp_batch_destroy(batch);siqs_tlp_prime_store_destroy(store);
  mpz_set_ui(product,0);
  assert(siqs_tlp_prime_store_from_product(&store,product,0,0,0)==SIQS_TLP_BAD_ARGUMENT&&store==NULL);
  for(i=0;i<5;++i){mpz_clear(input[i]);mpz_clear(gcds[i]);}mpz_clear(product);
}

/* Vary product/input sizes without depending on the chosen forest geometry.
 * Full and short calls reuse scratch; exact GCDs must also survive aliasing. */
static void rf_batch_gcd_geometry(void) {
  const unsigned long primes[]={2,3,5,7,11,13,17,19,23,29,31,37,41,43,47,53};
  const unsigned long values[]={2,3,25,55,77,101,169,323};
  const unsigned long powers[]={0,2,8};
  siqs_tlp_prime_store *store=NULL;siqs_tlp_batch *batch=NULL;
  mpz_t product,scale,input[8],saved[8],output[8],want[8];
  size_t i,capacity,counts[5],pass,k,count;unsigned empty,alias,small;
  mpz_inits(product,scale,NULL);
  for(i=0;i<8;++i)mpz_inits(input[i],saved[i],output[i],want[i],NULL);
  for(empty=0;empty<2;++empty) {
    mpz_set_ui(product,1);
    if(!empty)for(i=0;i<sizeof(primes)/sizeof(primes[0]);++i)
      mpz_mul_ui(product,product,primes[i]);
    assert(siqs_tlp_prime_store_from_product(&store,product,empty?0:2,
                                            empty?0:53,empty?0:16)==SIQS_TLP_OK);
    for(capacity=1;capacity<=8;capacity*=8) {
      assert(siqs_tlp_batch_create(&batch,store,capacity)==SIQS_TLP_OK);
      assert(siqs_tlp_batch_gcd(batch,NULL,NULL,0)==SIQS_TLP_OK);
      assert(siqs_tlp_batch_gcd_fb(batch,NULL,NULL,0)==SIQS_TLP_OK);
      counts[0]=capacity;counts[1]=capacity==1?1:5;
      counts[2]=1;counts[3]=capacity==1?0:7;counts[4]=capacity;
      for(pass=0;pass<sizeof(powers)/sizeof(powers[0]);++pass) {
        /* 101 lies outside the product; changing its exponent changes input
         * widths while retaining smooth, proper, repeated-prime and GCD-one cases. */
        mpz_ui_pow_ui(scale,101,powers[pass]);
        for(k=0;k<5;++k)for(alias=0;alias<2;++alias)for(small=0;small<2;++small) {
          count=counts[k];
          for(i=0;i<8;++i) {
            mpz_mul_ui(input[i],scale,values[i]);mpz_set(saved[i],input[i]);
            mpz_set_ui(output[i],1234567);mpz_gcd(want[i],product,input[i]);
          }
          /* Clear old ancestors so this case checks the target actually used. */
          if(!empty && capacity==8 && pass==0 && count==8)
            for(i=1;i<capacity;++i)mpz_set_ui(batch->product[i],0);
          if(small)
            assert(siqs_tlp_batch_gcd_fb(batch,input,alias?input:output,count)==SIQS_TLP_OK);
          else
            assert(siqs_tlp_batch_gcd(batch,input,alias?input:output,count)==SIQS_TLP_OK);
          if(!empty && capacity==8 && pass==0 && count==8) {
            assert((mpz_sgn(batch->product[1])==0)==small);
            assert(mpz_sgn(batch->product[2])>0);
          }
          for(i=0;i<count;++i)assert(mpz_cmp(alias?input[i]:output[i],want[i])==0);
          for(i=count;i<8;++i) {
            if(alias)assert(mpz_cmp(input[i],saved[i])==0);
            else assert(mpz_cmp_ui(output[i],1234567)==0);
          }
          if(!alias)for(i=0;i<8;++i)assert(mpz_cmp(input[i],saved[i])==0);
        }
      }
      siqs_tlp_batch_destroy(batch);
    }
    siqs_tlp_prime_store_destroy(store);
  }
  for(i=0;i<8;++i)mpz_clears(input[i],saved[i],output[i],want[i],NULL);
  mpz_clears(product,scale,NULL);
}

static void rf_stop_job(int factor_known) {
  siqs_ctx_t master;siqs_factor_array_t result;psiqs_pool_t *pool;
  siqs_raw_relation_t *packet;mpz_t n,norm,y;
  uint64_t jobs;
  mpz_init_set_ui(n,10403);mpz_init_set_ui(norm,4199);mpz_init_set_ui(y,991);
  rf_test_context(&master,&result,n,1);pool=tlp_job_test_pool(&master,NULL);
  pool->workers[0].ctx.total_candidates=555;
  if(factor_known) {
    /* Stage an LP tail before assigning a worker job. */
    packet=siqs_rf_packet_new(&master.rf->pending_arena,y,NULL,0,norm,NULL);
    siqs_rf_queue(&master,packet);siqs_rf_submit(&master,0);
    assert(master.rf->slots[0]->published_lp_count==1);
  }
  jobs=master.rf->jobs_done;
  packet=siqs_rf_packet_new(&master.rf->pending_arena,y,NULL,0,norm,NULL);
  siqs_rf_queue(&master,packet);
  pool->merging_worker=pool->workers;
  siqs_rf_submit(&master,factor_known ? 0 : 1);assert(master.rf->slots[0]->running);
  pool->merging_worker=NULL;
  if(factor_known) {
    /* A producer packet and the LP tail should be discarded on shutdown. */
    packet=siqs_rf_packet_new(&master.rf->pending_arena,y,NULL,0,norm,NULL);
    siqs_rf_queue(&master,packet);
    mpz_set_ui(norm,101);
    assert(siqs_insert_divisor(&result,norm));master.factor_found=1;
  }
  psiqs_pool_join(pool);siqs_rf_flush(&master);
  if(factor_known) {
    assert(master.rf->jobs_done==jobs+1);
    assert(master.accepted_three_lp==0 && master.full_count==0);
  } else {
    assert(master.accepted_three_lp==1 && master.full_count==1);
    siqs_verify_full_relation(&master,master.full[0]);
  }
  assert(pool->workers[0].ctx.total_candidates==555);
  assert(siqs_tlp_pending_count(&master)==0);
  if(pool->queued) {
    psiqs_worker_t *worker=psiqs_pool_take(pool);
    assert(worker->task==PSIQS_TASK_GCD);assert(psiqs_release_gcd_worker(&master,worker)==0);
  }
  tlp_job_test_pool_clear(pool);release_context(&master,&result);
  mpz_clears(n,norm,y,NULL);
}

/* Hold real pool executors at callback entry. This makes overlap, shutdown
 * during two active jobs, and out-of-order completion deterministic. */
typedef struct {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  unsigned arrived;
  int hold_all, release;
} rf_test_gate_t;

static void rf_test_started(void *data) {
  rf_test_gate_t *g=data; unsigned order;
  assert(pthread_mutex_lock(&g->mutex)==0);
  order=g->arrived++;
  assert(pthread_cond_broadcast(&g->changed)==0);
  while(!g->release && (g->hold_all || order==0))
    assert(pthread_cond_wait(&g->changed,&g->mutex)==0);
  assert(pthread_mutex_unlock(&g->mutex)==0);
}

static void rf_test_arrived(rf_test_gate_t *g,unsigned count) {
  struct timespec until;
  assert(clock_gettime(CLOCK_REALTIME,&until)==0);until.tv_sec+=10;
  assert(pthread_mutex_lock(&g->mutex)==0);
  while(g->arrived<count)
    assert(pthread_cond_timedwait(&g->changed,&g->mutex,&until)==0);
  assert(pthread_mutex_unlock(&g->mutex)==0);
}

static void rf_test_queue_case(siqs_ctx_t *master,siqs_ctx_t *normal,unsigned index) {
  mpz_t norm,y;siqs_factor_t known[4];uint32_t count;
  siqs_raw_relation_t *packet;
  mpz_inits(norm,y,NULL);rf_case(index,norm,y,known,&count);
  rf_trial_oracle(normal,norm,y,known,count);
  packet=siqs_rf_packet_new(&master->rf->pending_arena,y,known,count,norm,NULL);
  siqs_rf_queue(master,packet);mpz_clears(norm,y,NULL);
}

#ifndef SIQS_NO_JOURNAL
typedef struct {
  siqs_ctx_t *ctx;
  rf_test_gate_t *gate;
  int done;
} rf_checkpoint_test_t;

static void *rf_test_checkpoint(void *data) {
  rf_checkpoint_test_t *t=data;
  sj_maybe_flush(t->ctx);
  assert(pthread_mutex_lock(&t->gate->mutex)==0);
  t->done=1;assert(pthread_cond_broadcast(&t->gate->changed)==0);
  assert(pthread_mutex_unlock(&t->gate->mutex)==0);
  return NULL;
}

/* Hold a real executor with an LP tail and leave a producer packet pending.
 * A checkpoint must finish before releasing that job, and replay must recover
 * only the accepted prefix. Final attempt completion still drains everything. */
static void rf_journal_checkpoint(void) {
  siqs_ctx_t master,normal,replayed;siqs_factor_array_t result,nr,rr;
  siqs_journal_t j,reader;siqs_poly_t poly;
  rf_test_gate_t gate;rf_checkpoint_test_t checkpoint;psiqs_pool_t *pool;
  pthread_t writer;struct timespec until;uint64_t polys=0,families=0,replay_polys=0,jobs;
  FILE *snapshot;char bytes[4096];size_t got;unsigned i;int finished;
  mpz_t n,y;mpz_init_set_ui(n,10403);mpz_init_set_ui(y,991);
  rf_test_context(&master,&result,n,1);rf_test_context(&normal,&nr,n,0);
  rf_test_context(&replayed,&rr,n,0);
  memset(&j,0,sizeof(j));j.file=tmpfile();assert(j.file!=NULL);
  j.path=(char *)"active-RF-checkpoint";j.lp_bytes=1;
  master.journal=&j;master.journal_poly_count=&polys;
  master.la_rng.state=11;master.cofactor_rng.state=13;master.poly_rng.state=17;
  siqs_accept_raw_relation(&master,siqs_raw_relation_new3(&master,y,NULL,0,13,17,19));
  siqs_accept_raw_relation(&normal,siqs_raw_relation_new3(&normal,y,NULL,0,13,17,19));
  /* Complete one FB job without forcing its short LP queue. */
  rf_test_queue_case(&master,&normal,3);
  for(i=1;i<master.rf->fb_capacity;++i)rf_test_queue_case(&master,&normal,8);
  siqs_rf_finish(&master,1);
  assert(master.rf->slots[0]->published_lp_count==1 && master.full_count==1);
  memset(&gate,0,sizeof(gate));gate.hold_all=1;
  assert(pthread_mutex_init(&gate.mutex,NULL)==0);
  assert(pthread_cond_init(&gate.changed,NULL)==0);
  master.rf->started=rf_test_started;master.rf->started_data=&gate;
  pool=tlp_job_test_pool(&master,NULL);
  pool->merging_worker=pool->workers;pool->merging_dispatched=0;
  rf_test_queue_case(&master,&normal,2);
  for(i=1;i<master.rf->fb_capacity;++i)rf_test_queue_case(&master,&normal,8);
  pool->merging_worker=NULL;rf_test_arrived(&gate,1);
  rf_test_queue_case(&master,&normal,0);
  assert(master.rf->count==1 && master.rf->slots[0]->running);
  jobs=master.rf->jobs_done;
  j.clock_started=1;j.mono_start=sj_clock_us(&j,1);
  j.last_checkpoint=j.mono_start-UINT64_C(20000000);
  checkpoint.ctx=&master;checkpoint.gate=&gate;checkpoint.done=0;
  assert(pthread_create(&writer,NULL,rf_test_checkpoint,&checkpoint)==0);
  assert(clock_gettime(CLOCK_REALTIME,&until)==0);until.tv_sec+=5;
  assert(pthread_mutex_lock(&gate.mutex)==0);
  while(!checkpoint.done) {
    int status=pthread_cond_timedwait(&gate.changed,&gate.mutex,&until);
    assert(status==0 || status==ETIMEDOUT);
    if(status==ETIMEDOUT)break;
  }
  finished=checkpoint.done;
  /* Release on failure so the old draining implementation fails promptly. */
  if(!finished){gate.release=1;assert(pthread_cond_broadcast(&gate.changed)==0);}
  assert(pthread_mutex_unlock(&gate.mutex)==0);
  assert(pthread_join(writer,NULL)==0);assert(finished);
  assert(j.sequence==1 && j.batch.size==0 && master.full_count==1);
  assert(master.rf->jobs_done==jobs && master.rf->count==1);
  assert(master.rf->slots[0]->running && master.rf->slots[0]->published_lp_count==1);
  assert(siqs_rf_read_counter(master.rf,&master.rf->active_jobs)==1);
  /* Copy exactly the committed frame while the original executor stays held. */
  snapshot=tmpfile();assert(snapshot!=NULL);rewind(j.file);
  while((got=fread(bytes,1,sizeof(bytes),j.file))!=0)assert(fwrite(bytes,1,got,snapshot)==got);
  assert(!ferror(j.file));assert(fseek(j.file,0,SEEK_END)==0);rewind(snapshot);
  memset(&reader,0,sizeof(reader));reader.file=snapshot;reader.reading=1;
  reader.replaying=1;reader.path=(char *)"replayed-RF-checkpoint";reader.lp_bytes=1;
  replayed.journal=&reader;memset(&poly,0,sizeof(poly));
  assert(sj_peek(&reader) && reader.frame_type==SJ_BATCH);
  sj_replay_batch(&replayed,&poly,&families,&replay_polys);
  assert(families==0 && replay_polys==0 && replayed.full_count==1);
  assert(replayed.accepted_three_lp==1 && siqs_tlp_pending_count(&replayed)==0);
  assert(replayed.la_rng.state==11 && replayed.cofactor_rng.state==13 && replayed.poly_rng.state==17);
  siqs_verify_full_relation(&replayed,replayed.full[0]);
  assert(!sj_peek(&reader));replayed.journal=NULL;assert(fclose(snapshot)==0);
  /* One new relation completes the replayed dependency and finds 101 * 103. */
  mpz_set_ui(y,2948);
  siqs_accept_raw_relation(&replayed,siqs_raw_relation_new3(&replayed,y,NULL,0,13,17,19));
  {
    unsigned long nrows,ncols;uint64_t mask,*nullrows;la_col_t *columns;
    columns=siqs_lp_combiner_matrix(&replayed,&nrows,&ncols);
    nullrows=la_dense_nullspace(nrows,ncols,columns,&mask);assert(nullrows!=NULL && mask!=0);
    assert(siqs_test_dependencies(&replayed,columns,ncols,nullrows,mask));
    assert(rr.count==2 && siqs_all_factors_prime(&rr));siqs_verify_partition(n,&rr);
    for(i=0;i<ncols;++i)free(columns[i].data);free(columns);free(nullrows);
  }
  assert(pthread_mutex_lock(&pool->mutex)==0);pool->stop=1;
  assert(pthread_cond_signal(&pool->workers[0].work)==0);
  assert(pthread_mutex_unlock(&pool->mutex)==0);
  assert(pthread_mutex_lock(&gate.mutex)==0);gate.release=1;
  assert(pthread_cond_broadcast(&gate.changed)==0);
  assert(pthread_mutex_unlock(&gate.mutex)==0);psiqs_pool_join(pool);
  sj_end_attempt(&master);siqs_tlp_flush(&normal);
  assert(j.batch.size==0 && siqs_tlp_pending_count(&master)==0);rf_equal(&master,&normal);
  while(pool->queued) {
    psiqs_worker_t *worker=psiqs_pool_take(pool);
    assert(psiqs_release_gcd_worker(&master,worker)==0);
  }
  master.rf->started=NULL;master.rf->started_data=NULL;master.journal=NULL;
  assert(pthread_cond_destroy(&gate.changed)==0);assert(pthread_mutex_destroy(&gate.mutex)==0);
  free(j.batch.data);assert(fclose(j.file)==0);tlp_job_test_pool_clear(pool);
  release_context(&master,&result);release_context(&normal,&nr);release_context(&replayed,&rr);
  mpz_clears(n,y,NULL);
  puts("{\"checkpoint_with_active_RF\":true,\"pending_LP_and_producer_preserved\":true,\"accepted_prefix_replay_and_factor\":true,\"attempt_end_drains_RF\":true}");
}
#endif

static void rf_concurrent_stop(int hold_all) {
  siqs_ctx_t master,normal;siqs_factor_array_t result,nr;mpz_t n;
  rf_test_gate_t gate;psiqs_pool_t *pool;unsigned i;
  mpz_init_set_ui(n,10403);rf_test_context(&master,&result,n,1);
  rf_test_context(&normal,&nr,n,0);assert(master.rf->job_count==1 && master.rf->job_limit==4);
  memset(&gate,0,sizeof(gate));gate.hold_all=hold_all;
  assert(pthread_mutex_init(&gate.mutex,NULL)==0);
  assert(pthread_cond_init(&gate.changed,NULL)==0);
  master.rf->started=rf_test_started;master.rf->started_data=&gate;
  pool=tlp_job_test_pool_count(&master,NULL,2);
  for(i=0;i<2;++i) {
    pool->workers[i].ctx.total_candidates=555+i;
    pool->workers[i].ctx.cofactor_rng.state=777+i;
  }
  for(i=0;i<2;++i) {
    pool->merging_worker=pool->workers+i;pool->merging_dispatched=0;
    rf_test_queue_case(&master,&normal,i);
    rf_test_queue_case(&master,&normal,3+i);
    for(size_t pad=2;pad<master.rf->fb_capacity;++pad) rf_test_queue_case(&master,&normal,8);
    siqs_rf_submit(&master,0);pool->merging_worker=NULL;
    assert(master.rf->slots[i]->running);rf_test_arrived(&gate,i+1);
  }
  assert(master.rf->job_count==2);
  assert(siqs_rf_read_counter(master.rf,&master.rf->peak_active_jobs)==2);
  assert(master.full_count==0); /* Workers do not publish graph results. */
  assert(master.rf->slots[0]->fb_store==master.rf->slots[1]->fb_store);
  assert(master.rf->slots[0]->tree==master.rf->slots[1]->tree);
  assert(master.rf->slots[0]->resolver.tlp->store==master.rf->slots[1]->resolver.tlp->store);
  assert(master.rf->slots[0]->fb_batch!=master.rf->slots[1]->fb_batch);
  assert(master.rf->slots[0]->resolver.tlp->batch!=master.rf->slots[1]->resolver.tlp->batch);
  assert(master.rf->slots[0]->inputs!=master.rf->slots[1]->inputs);
  assert(master.rf->slots[0]->lp_pending!=master.rf->slots[1]->lp_pending);
  if(!hold_all) {
    psiqs_worker_t *worker;
    /* The newer slot can publish and run again while the oldest is held. */
    siqs_rf_finish_slot(&master,master.rf->slots[1],1);
    assert(master.full_count==1 && master.rf->slots[0]->running);
    worker=psiqs_pool_take(pool);assert(worker->index==1);
    assert(psiqs_release_gcd_worker(&master,worker)==0);
    pool->merging_worker=worker;pool->merging_dispatched=0;
    rf_test_queue_case(&master,&normal,2);
    rf_test_queue_case(&master,&normal,7);
    for(size_t pad=2;pad<master.rf->fb_capacity;++pad) rf_test_queue_case(&master,&normal,8);
    siqs_rf_submit(&master,0);pool->merging_worker=NULL;
    siqs_rf_finish_slot(&master,master.rf->slots[1],1);
    assert(master.full_count==2 && master.rf->slots[0]->running);
  } else assert(siqs_rf_read_counter(master.rf,&master.rf->active_jobs)==2);
  assert(pthread_mutex_lock(&pool->mutex)==0);pool->stop=1;
  for(i=0;i<2;++i)assert(pthread_cond_signal(&pool->workers[i].work)==0);
  assert(pthread_mutex_unlock(&pool->mutex)==0);
  assert(pthread_mutex_lock(&gate.mutex)==0);gate.release=1;
  assert(pthread_cond_broadcast(&gate.changed)==0);
  assert(pthread_mutex_unlock(&gate.mutex)==0);
  psiqs_pool_join(pool);siqs_rf_flush(&master);siqs_tlp_flush(&normal);
  assert(siqs_rf_read_counter(master.rf,&master.rf->active_jobs)==0);
  assert(siqs_tlp_pending_count(&master)==0);rf_equal(&master,&normal);
  for(i=0;i<2;++i) {
    assert(pool->workers[i].ctx.total_candidates==555+i);
    assert(pool->workers[i].ctx.cofactor_rng.state==777+i);
    assert(pool->workers[i].result.count==1);
  }
  while(pool->queued) {
    psiqs_worker_t *worker=psiqs_pool_take(pool);
    assert(worker->task==PSIQS_TASK_GCD);
    assert(psiqs_release_gcd_worker(&master,worker)==0);
  }
  master.rf->started=NULL;master.rf->started_data=NULL;
  assert(pthread_cond_destroy(&gate.changed)==0);
  assert(pthread_mutex_destroy(&gate.mutex)==0);
  tlp_job_test_pool_clear(pool);release_context(&master,&result);
  release_context(&normal,&nr);mpz_clear(n);
}

/* The largest scratch tree can serve both stores because the phases do not
 * overlap within a context. Check repeated FB -> LP -> FB rebinding against
 * direct GMP GCDs; scratch must never leak a previous store into the result. */
/* Exercise recovery across several tree depths, then shrink and reuse it.
 * Scalar division supplies the exponent oracle independently of the tree. */
static void rf_recovery_reuse(void) {
  enum { FB_COUNT=131 };
  const unsigned spans[]={1,2,31,32,33,65,129,1,0,33,129};
  const uint64_t label=20807; /* Prime, 1 mod 10403, above this factor base. */
  siqs_ctx_t ctx;siqs_factor_array_t result;struct siqs_rf_state *s;
  siqs_rf_accepted_t accepted[2];siqs_raw_arena_t arena;
  siqs_factor_t known[2][3];uint32_t expected[2][FB_COUNT+1];
  mpz_t n,norm[2],y[2],distinct,power,rest,check;
  pr32_iter_t iter;unsigned round,j,i;uint32_t before,nknown[2],lp[2];
  mpz_init_set_ui(n,10403);mpz_inits(distinct,power,rest,check,NULL);
  for(j=0;j<2;++j){mpz_init(norm[j]);mpz_init(y[j]);}
  rf_test_context_threads(&ctx,&result,n,0,1);
  siqs_tlp_clear(&ctx);siqs_lp_combiner_clear(&ctx);
  ctx.fb=siqs_realloc(ctx.fb,FB_COUNT*sizeof(*ctx.fb));
  ctx.prime=siqs_realloc(ctx.prime,FB_COUNT*sizeof(*ctx.prime));
  memset(ctx.fb,0,FB_COUNT*sizeof(*ctx.fb));pr32_iter_init(&iter);
  for(i=0;i<FB_COUNT;++i)ctx.fb[i].p=ctx.prime[i]=pr32_iter_next(&iter);
  pr32_iter_destroy(&iter);ctx.params.fb_size=FB_COUNT;
  ctx.largest_fb_prime=ctx.prime[FB_COUNT-1];
  ctx.params.large_prime_bound=label;ctx.params.smooth_bound=label*label;
  ctx.params.tlp_smooth_bound=label*label*label;ctx.params.tlp_cutoff=(uint32_t)label;
  siqs_mpz_set_u64(ctx.tlp_upper,ctx.params.tlp_smooth_bound);
  siqs_tlp_policy_clear(&ctx);siqs_tlp_policy_init(&ctx);
  free(ctx.factor_counts);free(ctx.factor_touched);ctx.factor_row_alloc=FB_COUNT+1;
  ctx.factor_counts=siqs_calloc(FB_COUNT+1,sizeof(*ctx.factor_counts));
  ctx.factor_touched=siqs_malloc((FB_COUNT+1)*sizeof(*ctx.factor_touched));
  ctx.residual_first=1;
  siqs_lp_combiner_init(&ctx);siqs_tlp_init(&ctx);siqs_rf_init(&ctx);
  s=ctx.rf->slots[0];siqs_rf_recover(s,NULL,0);assert(s->resolver.raw_count==0);
  for(round=0;round<sizeof(spans)/sizeof(*spans);++round) {
    unsigned span=spans[round];
    before=s->resolver.raw_count;memset(&arena,0,sizeof(arena));
    memset(expected,0,sizeof(expected));
    for(j=0;j<2;++j) {
      uint32_t single=span?2U+(17U*round)%span:0;
      mpz_set_ui(norm[j],1);mpz_set_ui(distinct,1);mpz_set_ui(y[j],6);
      known[j][0].row=1;known[j][1].row=2;
      known[j][0].exponent=known[j][1].exponent=2;nknown[j]=2;
      if(j==0&&span) {
        known[j][2].row=single+1;known[j][2].exponent=2;nknown[j]=3;
        mpz_mul_ui(y[j],y[j],ctx.prime[single]);
      }
      for(i=2;i<span+2U;++i)if(j||i==single) {
        unsigned exponent=j?2U:round==2?1024U:2U*(1U+round%3U);
        mpz_mul_ui(distinct,distinct,ctx.prime[i]);
        mpz_ui_pow_ui(power,ctx.prime[i],exponent);mpz_mul(norm[j],norm[j],power);
        mpz_ui_pow_ui(power,ctx.prime[i],exponent/2U);mpz_mul(y[j],y[j],power);
      }
      lp[j]=(round+j)%4U;mpz_ui_pow_ui(power,(unsigned long)label,lp[j]);
      mpz_mul(norm[j],norm[j],power);mpz_mod(y[j],y[j],n);
      for(i=0;i<nknown[j];++i)expected[j][known[j][i].row]+=known[j][i].exponent;
      mpz_set(rest,norm[j]);
      for(i=2;i<FB_COUNT;++i)while(mpz_divisible_ui_p(rest,ctx.prime[i])) {
        mpz_divexact_ui(rest,rest,ctx.prime[i]);++expected[j][i+1U];
      }
      assert(mpz_cmp(rest,power)==0);
      accepted[j].packet=siqs_rf_packet_new(&arena,y[j],known[j],nknown[j],norm[j],distinct);
      accepted[j].count=lp[j];
      for(i=0;i<3;++i)accepted[j].labels[i]=i<lp[j]?label:1;
    }
    siqs_rf_recover(s,accepted,2);assert(s->resolver.raw_count==before+2);
    for(j=0;j<2;++j)siqs_rf_packet_clear(accepted[j].packet);
    siqs_raw_arena_clear(&arena); /* Recovered records must own their values. */
    for(j=0;j<2;++j) {
      siqs_raw_relation_t *r=s->resolver.raw[before+j];uint32_t got[FB_COUNT+1]={0};
      for(i=0;i<r->nfactors;++i) {
        uint32_t row=siqs_raw_factor_row(r,i);assert(row<=FB_COUNT);
        got[row]+=siqs_raw_factor_exponent(r,i);
      }
      for(i=0;i<=FB_COUNT;++i)assert(got[i]==expected[j][i]);
      assert(mpz_cmp(r->y,y[j])==0&&r->rf_norm==NULL);
      assert(r->lp1==(lp[j]>=2?label:1)&&r->lp2==(lp[j]?label:1));
      assert(r->lp3==(lp[j]==3?label:1));
      mpz_set_ui(check,1);
      for(i=1;i<=FB_COUNT;++i)if(got[i]) {
        mpz_ui_pow_ui(power,ctx.prime[i-1U],got[i]);mpz_mul(check,check,power);
      }
      mpz_ui_pow_ui(power,(unsigned long)label,lp[j]);mpz_mul(check,check,power);
      mpz_mod(check,check,n);mpz_mul(rest,r->y,r->y);mpz_mod(rest,rest,n);
      assert(mpz_cmp(check,rest)==0);
    }
    siqs_rf_recover(s,NULL,0);assert(s->resolver.raw_count==before+2);
  }
  release_context(&ctx,&result);
  for(j=0;j<2;++j){mpz_clear(norm[j]);mpz_clear(y[j]);}
  mpz_clears(n,distinct,power,rest,check,NULL);
}

static void rf_workspace_reuse(void) {
  siqs_ctx_t ctx;siqs_factor_array_t result;mpz_t n,product,want;
  const unsigned long values[]={35,221,7315,1009,1001};
  mpz_t input[32],output[32];unsigned round,i;
  mpz_init_set_ui(n,10403);mpz_inits(product,want,NULL);
  rf_test_context(&ctx,&result,n,1);
  struct siqs_rf_state *s=ctx.rf->slots[0];
  assert(ctx.rf->job_count==1 && ctx.rf->job_limit==4);
  assert(s->direct==NULL && s->lp_accepted==NULL);
  for(i=1;i<ctx.rf->job_limit;++i)assert(ctx.rf->slots[i]==NULL);
  assert(s->fb_batch==s->resolver.tlp->batch);
  for(i=0;i<32;++i){mpz_init_set_ui(input[i],values[i%5]);mpz_init(output[i]);}
  for(round=0;round<6;++round) {
    int lp=round&1U;
    siqs_tlp_prime_store *store=lp?ctx.rf->lp_store:ctx.rf->fb_store;
    siqs_tlp_batch *batch=siqs_rf_batch(s,lp,32);
    assert(batch==s->fb_batch);
    mpz_set_ui(product,1);
    for(i=0;i<store->stats.block_count;++i)mpz_mul(product,product,store->blocks[i]);
    assert(siqs_tlp_batch_gcd(batch,input,output,32)==SIQS_TLP_OK);
    for(i=0;i<32;++i){mpz_gcd(want,input[i],product);assert(mpz_cmp(want,output[i])==0);}
  }
  /* A smaller logical LP capacity fits the already allocated value buffers.
   * Small LP tails must leave its separate full workspace unallocated. */
  ctx.rf->lp_capacity=16;s->resolver.tlp->batch=NULL;
  assert(siqs_tlp_batch_capacity(siqs_rf_batch(s,1,1))==1);
  assert(s->resolver.tlp->batch==NULL);
  siqs_tlp_batch *separate=siqs_rf_batch(s,1,16);
  assert(separate!=s->fb_batch && siqs_tlp_batch_capacity(separate)==16);
  assert(siqs_tlp_batch_gcd(separate,input,output,16)==SIQS_TLP_OK);
  for(i=0;i<16;++i){mpz_gcd(want,input[i],product);assert(mpz_cmp(want,output[i])==0);}
  assert(siqs_rf_batch(s,0,32)==s->fb_batch);
  for(i=0;i<32;++i){mpz_clear(input[i]);mpz_clear(output[i]);}
  release_context(&ctx,&result);mpz_clears(n,product,want,NULL);
}

static void rf_lazy_directory(void) {
  const unsigned counts[]={1,48,PSIQS_MAX_THREADS};
  for(unsigned which=0;which<sizeof(counts)/sizeof(*counts);++which) {
    siqs_ctx_t ctx,normal;siqs_factor_array_t result,nr;mpz_t n;
    mpz_init_set_ui(n,10403);
    rf_test_context_threads(&ctx,&result,n,1,counts[which]);
    rf_test_context(&normal,&nr,n,0);
    assert(ctx.rf->job_count==1 && ctx.rf->job_limit==counts[which]);
    for(unsigned i=1;i<counts[which];++i)assert(ctx.rf->slots[i]==NULL);
    /* No executor callback: even a large allowed pool stays synchronous and
     * reuses its one context when there is no actual execution pressure. */
    for(size_t i=0;i<2*ctx.rf->fb_capacity+1;++i)
      rf_test_queue_case(&ctx,&normal,(unsigned)i);
    siqs_rf_flush(&ctx);siqs_tlp_flush(&normal);
    assert(ctx.rf->job_count==1 && siqs_tlp_pending_count(&ctx)==0);
    rf_equal(&ctx,&normal);
    release_context(&ctx,&result);release_context(&normal,&nr);mpz_clear(n);
  }
  puts("{\"one_worker_synchronous\":true,\"lazy_directory_up_to_256\":true}");
}

/* Force genuine concurrent callbacks beyond the former two-slot limit.
 * Every callback is held while the directory grows, so a moved or borrowed
 * mutable context would fail deterministically. Then stop with every job
 * active, drain LP tails and compare exact relations with trial division. */
static void rf_dynamic_stop(unsigned threads) {
  siqs_ctx_t master,normal;siqs_factor_array_t result,nr;mpz_t n;
  rf_test_gate_t gate;psiqs_pool_t *pool;unsigned i,j;
  struct siqs_rf_state *first;
  mpz_init_set_ui(n,10403);
  rf_test_context_threads(&master,&result,n,1,threads);
  rf_test_context(&normal,&nr,n,0);
  assert(master.rf->job_count==1 && master.rf->job_limit==threads);
  first=master.rf->slots[0];
  for(i=1;i<threads;++i)assert(master.rf->slots[i]==NULL);
  memset(&gate,0,sizeof(gate));gate.hold_all=1;
  assert(pthread_mutex_init(&gate.mutex,NULL)==0);
  assert(pthread_cond_init(&gate.changed,NULL)==0);
  master.rf->started=rf_test_started;master.rf->started_data=&gate;
  pool=tlp_job_test_pool_count(&master,NULL,threads);
  for(i=0;i<threads;++i) {
    pool->workers[i].ctx.total_candidates=555+i;
    pool->workers[i].ctx.cofactor_rng.state=777+i;
    pool->merging_worker=pool->workers+i;pool->merging_dispatched=0;
    rf_test_queue_case(&master,&normal,i);
    for(j=1;j<master.rf->fb_capacity;++j)rf_test_queue_case(&master,&normal,8);
    pool->merging_worker=NULL;
    rf_test_arrived(&gate,i+1);
    assert(master.rf->job_count==i+1 && master.rf->slots[0]==first);
    struct siqs_rf_state *s=master.rf->slots[i];
    assert(s->running && s->owner==&master);
    assert(s->fb_store==first->fb_store && s->tree==first->tree);
    assert(s->resolver.tlp->store==first->resolver.tlp->store);
    assert(s->fb_batch==s->resolver.tlp->batch);
    if(i)assert(s->fb_batch!=first->fb_batch && s->inputs!=first->inputs &&
                s->lp_pending!=first->lp_pending);
  }
  assert(siqs_rf_read_counter(master.rf,&master.rf->peak_active_jobs)==threads);
  assert(master.full_count==0);
  assert(pthread_mutex_lock(&pool->mutex)==0);pool->stop=1;
  for(i=0;i<threads;++i)assert(pthread_cond_signal(&pool->workers[i].work)==0);
  assert(pthread_mutex_unlock(&pool->mutex)==0);
  assert(pthread_mutex_lock(&gate.mutex)==0);gate.release=1;
  assert(pthread_cond_broadcast(&gate.changed)==0);
  assert(pthread_mutex_unlock(&gate.mutex)==0);
  psiqs_pool_join(pool);siqs_rf_flush(&master);siqs_tlp_flush(&normal);
  assert(master.rf->job_count==threads && siqs_tlp_pending_count(&master)==0);
  assert(siqs_rf_read_counter(master.rf,&master.rf->active_jobs)==0);
  rf_equal(&master,&normal);
  for(i=0;i<threads;++i) {
    assert(pool->workers[i].ctx.total_candidates==555+i);
    assert(pool->workers[i].ctx.cofactor_rng.state==777+i);
    assert(pool->workers[i].result.count==1);
  }
  while(pool->queued) {
    psiqs_worker_t *worker=psiqs_pool_take(pool);
    assert(worker->task==PSIQS_TASK_GCD);
    assert(psiqs_release_gcd_worker(&master,worker)==0);
  }
  master.rf->started=NULL;master.rf->started_data=NULL;
  assert(pthread_cond_destroy(&gate.changed)==0);
  assert(pthread_mutex_destroy(&gate.mutex)==0);
  tlp_job_test_pool_clear(pool);release_context(&master,&result);
  release_context(&normal,&nr);mpz_clear(n);
  printf("{\"dynamic_pool_overlap\":%u,\"lazy_contexts\":true,\"stable_addresses\":true,\"stop_and_LP_tail_drain\":true}\n",threads);
}

int main(int argc,char **argv) {
  assert(existing_private_checks(argc,argv)==0);
  rf_batch_sizes();rf_stop_job(0);rf_stop_job(1);rf_product_store();rf_batch_gcd_geometry();
#ifndef SIQS_NO_JOURNAL
  rf_journal_checkpoint();
#endif
  rf_concurrent_stop(1);rf_concurrent_stop(0);
  rf_recovery_reuse();rf_workspace_reuse();rf_lazy_directory();
  rf_dynamic_stop(3);rf_dynamic_stop(8);rf_dynamic_stop(48);
  puts("{\"two_jobs_overlap\":true,\"private_mutable_scratch\":true,\"shared_products\":true,\"two_active_stop\":true,\"out_of_order_reuse\":true}");
  puts("{\"residual_trial_oracle\":true,\"wide_norms\":true,\"known_and_recovered_exponents\":true,\"norm_one\":true,\"partial_and_LP_only_drains\":true,\"callback_stop_and_ownership\":true}");
  return 0;
}
