/* GCD-job ownership and publication checks, included after siqs.c. */
#ifdef PSIQS
#include <sched.h>

typedef struct {
  pthread_mutex_t mutex;
  pthread_cond_t changed;
  siqs_tlp_gcd_job_t *job;
  int arrived, release;
} tlp_job_test_gate_t;

static void *tlp_job_test_gated_run(void *argument) {
  tlp_job_test_gate_t *gate = argument;
  assert(pthread_mutex_lock(&gate->mutex) == 0);
  gate->arrived = 1;
  assert(pthread_cond_broadcast(&gate->changed) == 0);
  while (!gate->release)
    assert(pthread_cond_wait(&gate->changed, &gate->mutex) == 0);
  assert(pthread_mutex_unlock(&gate->mutex) == 0);
  siqs_tlp_gcd_job_run(gate->job);
  return NULL;
}

static void *tlp_job_test_run(void *argument) {
  siqs_tlp_gcd_job_run(argument);
  return NULL;
}

/* Multiplying a number congruent to one modulo P by a selected divisor of
 * P gives a known GCD while exercising more than one GMP limb. */
static void tlp_job_test_fill(mpz_t *values, size_t count, unsigned shift) {
  static const unsigned long divisors[] = {4199, 13, 323, 1, 19, 17, 221, 247};
  size_t i;
  for (i = 0; i < count; ++i) {
    mpz_set_ui(values[i], 13UL * 17UL * 19UL);
    mpz_mul_2exp(values[i], values[i], shift + (unsigned)i);
    mpz_add_ui(values[i], values[i], 1);
    mpz_mul_ui(values[i], values[i], divisors[i % 8]);
  }
}

static void tlp_job_test_answers(const mpz_t *inputs, const mpz_t *gcds,
                                   size_t count) {
  mpz_t product, wanted;
  size_t i;
  mpz_init_set_ui(product, 13UL * 17UL * 19UL);
  mpz_init(wanted);
  for (i = 0; i < count; ++i) {
    mpz_gcd(wanted, inputs[i], product);
    assert(mpz_cmp(wanted, gcds[i]) == 0);
  }
  mpz_clears(product, wanted, NULL);
}

static void tlp_gcd_job_kernels(void) {
  enum { capacity = 32, tail = 5 };
  siqs_tlp_prime_store *store = NULL;
  siqs_tlp_batch *batch = NULL;
  siqs_tlp_gcd_job_t job;
  siqs_tlp_gcd_job_result_t result;
  tlp_job_test_gate_t gate;
  pthread_t thread;
  mpz_t inputs[capacity], gcds[capacity], producer[capacity];
  size_t i;
  int state;

  memset(&job, 0, sizeof(job));
  memset(&result, 0, sizeof(result));
  result.status = -7;
  assert(siqs_tlp_gcd_job_clear(NULL));
  assert(siqs_tlp_gcd_job_clear(&job));
  assert(siqs_tlp_gcd_job_clear(&job));
  assert(!siqs_tlp_gcd_job_init(&job, NULL, 0));
  assert(siqs_tlp_gcd_job_clear(&job));
  assert(!siqs_tlp_gcd_job_ready(&job));
  assert(!siqs_tlp_gcd_job_prepare(&job, NULL, NULL, 0));
  assert(!siqs_tlp_gcd_job_wait(&job, &result) && result.status == -7);
  siqs_tlp_gcd_job_run(&job);
  assert(siqs_tlp_prime_store_create(&store, 11, 19) == SIQS_TLP_OK);
  assert(siqs_tlp_batch_create(&batch, store, capacity) == SIQS_TLP_OK);
  assert(siqs_tlp_gcd_job_init(&job, batch, 0));
  for (i = 0; i < capacity; ++i) {
    mpz_init(inputs[i]); mpz_init_set_ui(gcds[i], 99991);
    mpz_init(producer[i]);
  }
  tlp_job_test_fill(inputs, capacity, 128);

  /* A real executor is parked before run, allowing deterministic checks of
   * queued ownership and a separate producer buffer without timing sleeps. */
  memset(&gate, 0, sizeof(gate));
  gate.job = &job;
  assert(pthread_mutex_init(&gate.mutex, NULL) == 0);
  assert(pthread_cond_init(&gate.changed, NULL) == 0);
  assert(siqs_tlp_gcd_job_prepare(&job, inputs, gcds, capacity));
  assert(pthread_create(&thread, NULL, tlp_job_test_gated_run, &gate) == 0);
  assert(pthread_mutex_lock(&gate.mutex) == 0);
  while (!gate.arrived)
    assert(pthread_cond_wait(&gate.changed, &gate.mutex) == 0);
  assert(!siqs_tlp_gcd_job_ready(&job));
  assert(!siqs_tlp_gcd_job_prepare(&job, producer, gcds, tail));
  assert(!siqs_tlp_gcd_job_clear(&job));
  tlp_job_test_fill(producer, capacity, 257);
  gate.release = 1;
  assert(pthread_cond_broadcast(&gate.changed) == 0);
  assert(pthread_mutex_unlock(&gate.mutex) == 0);
  assert(siqs_tlp_gcd_job_wait(&job, &result));
  assert(result.status == SIQS_TLP_OK && result.gcd_us == 0 && result.cpu_us == 0);
  /* Read outputs before join: wait, rather than joining the executor, must
   * establish visibility of the computed GMP limbs. */
  tlp_job_test_answers(inputs, gcds, capacity);
  assert(pthread_join(thread, NULL) == 0);
  assert(pthread_cond_destroy(&gate.changed) == 0);
  assert(pthread_mutex_destroy(&gate.mutex) == 0);

  /* A shorter second batch reuses the one scratch object. Direct run is the
   * coordinator fallback when no pool executor can accept a prepared job. */
  for (i = 0; i < capacity; ++i) mpz_set_ui(gcds[i], 99991);
  assert(siqs_tlp_gcd_job_prepare(&job, producer, gcds, tail));
  siqs_tlp_gcd_job_run(&job);
  assert(siqs_tlp_gcd_job_ready(&job));
  assert(!siqs_tlp_gcd_job_prepare(&job, inputs, gcds, capacity));
  siqs_tlp_gcd_job_run(&job); /* DONE cannot accidentally execute twice. */
  assert(siqs_tlp_gcd_job_wait(&job, &result) && result.status == SIQS_TLP_OK);
  tlp_job_test_answers(producer, gcds, tail);
  for (i = tail; i < capacity; ++i) assert(mpz_cmp_ui(gcds[i], 99991) == 0);
  assert(!siqs_tlp_gcd_job_ready(&job));
  result.status = -7;
  assert(!siqs_tlp_gcd_job_wait(&job, &result) && result.status == -7);

  /* The batch layer rejects 1 before writing any outputs. Preserve that
   * exact error through the asynchronous result channel. */
  mpz_set_ui(inputs[3], 1);
  for (i = 0; i < capacity; ++i) mpz_set_ui(gcds[i], 99991);
  assert(siqs_tlp_gcd_job_prepare(&job, inputs, gcds, capacity));
  assert(pthread_create(&thread, NULL, tlp_job_test_run, &job) == 0);
  assert(siqs_tlp_gcd_job_wait(&job, &result));
  assert(result.status == SIQS_TLP_BAD_ARGUMENT);
  for (i = 0; i < capacity; ++i) assert(mpz_cmp_ui(gcds[i], 99991) == 0);
  assert(pthread_join(thread, NULL) == 0);
  assert(siqs_tlp_gcd_job_prepare(&job, NULL, NULL, 0));
  siqs_tlp_gcd_job_run(&job);
  assert(siqs_tlp_gcd_job_wait(&job, &result) && result.status == SIQS_TLP_OK);
  assert(siqs_tlp_gcd_job_clear(&job));

  /* Enable optional clocks and clear without consuming the result. Bigger
   * limbs let the coordinator overlap teardown with real GCD work; observing
   * RUNNING or DONE under the mutex is safe even on a busy test host. clear
   * must publish complete outputs before allowing their destruction. */
  assert(siqs_tlp_gcd_job_init(&job, batch, 1));
  tlp_job_test_fill(inputs, capacity, 65536);
  assert(siqs_tlp_gcd_job_prepare(&job, inputs, gcds, capacity));
  assert(pthread_create(&thread, NULL, tlp_job_test_run, &job) == 0);
  assert(siqs_tlp_gcd_job_wait(&job, &result) && result.status == SIQS_TLP_OK);
#ifdef SIQS_TIMING
  assert(result.gcd_us > 0 && result.cpu_us > 0);
#else
  assert(result.gcd_us == 0 && result.cpu_us == 0);
#endif
  tlp_job_test_answers(inputs, gcds, capacity);
  assert(pthread_join(thread, NULL) == 0);
  assert(siqs_tlp_gcd_job_prepare(&job, inputs, gcds, capacity));
  assert(pthread_create(&thread, NULL, tlp_job_test_run, &job) == 0);
  do {
    assert(pthread_mutex_lock(&job.mutex) == 0);
    state = job.state;
    assert(pthread_mutex_unlock(&job.mutex) == 0);
    if (state == SIQS_TLP_GCD_JOB_PREPARED) sched_yield();
  } while (state == SIQS_TLP_GCD_JOB_PREPARED);
  assert(state == SIQS_TLP_GCD_JOB_RUNNING || state == SIQS_TLP_GCD_JOB_DONE);
  assert(siqs_tlp_gcd_job_clear(&job));
  tlp_job_test_answers(inputs, gcds, capacity);
  assert(pthread_join(thread, NULL) == 0);
  assert(siqs_tlp_gcd_job_clear(&job));
  for (i = 0; i < capacity; ++i) {
    mpz_clear(inputs[i]); mpz_clear(gcds[i]); mpz_clear(producer[i]);
  }
  siqs_tlp_batch_destroy(batch);
  siqs_tlp_prime_store_destroy(store);
}
#else
static void tlp_gcd_job_kernels(void) { }
#endif
