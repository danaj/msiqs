/* Exercise the host's wire admission and retry loop without opening TCP
 * listeners. The optional factoring test joins real managers over socketpairs. */
#define _POSIX_C_SOURCE 200809L
#ifdef NDEBUG
# undef NDEBUG
#endif
#include <assert.h>
#include <sys/socket.h>
#include <unistd.h>
#include "../lanczos.h"

static uint64_t *retry_lanczos(unsigned long, unsigned long, unsigned long,
                               la_col_t *, uint64_t, uint64_t *, int, int, uint32_t);
#define la_block_lanczos_threaded retry_lanczos
#include "../siqs.c"
#undef la_block_lanczos_threaded

typedef struct {
  ds_host_state *host;
  uint64_t before, after;
  unsigned failures;
} retry_fixture;

static _Thread_local retry_fixture *retry_test;

static uint64_t *retry_lanczos(unsigned long rows, unsigned long dense_rows,
                               unsigned long cols, la_col_t *entries,
                               uint64_t seed, uint64_t *mask, int retain_all_rows,
                               int verbose, uint32_t threads) {
  if (retry_test != NULL) {
    ds_host_state *host = retry_test->host;
    size_t i;
    assert(host->reserved == 0);
    for (i = 0; i < host->count; ++i)
      assert(host->remotes[i]->link == DS_LINK_PAUSED);
    if (retry_test->failures < 2) {
      if (retry_test->failures == 0) retry_test->before = host->next_id;
      assert(host->next_id == retry_test->before);
      assert(retain_all_rows == (int)retry_test->failures);
      ++retry_test->failures;
      *mask = 0;
      return NULL;
    }
    if (retry_test->after == 0) retry_test->after = host->next_id;
  }
  return la_block_lanczos_threaded(rows, dense_rows, cols, entries, seed,
                                    mask, retain_all_rows, verbose, threads);
}

static void encode_triple(siqs_ctx_t *ctx, ds_buf *batch,
                          const unsigned long *roots, uint32_t count) {
  uint32_t i;
  mpz_t root;
  batch->size = 0;
  ds_put_u32(batch, count);
  mpz_init(root);
  for (i = 0; i < count; ++i) {
    siqs_raw_relation_t *relation;
    mpz_set_ui(root, roots[i]);
    relation = siqs_raw_relation_new3(ctx, root, NULL, 0, 13, 17, 19);
    assert(ds_encode_relation(batch, relation));
    siqs_raw_relation_free(ctx, relation);
  }
  mpz_clear(root);
}

static int admit(ds_collection *collection, ds_remote *remote, ds_buf *batch) {
  const ds_frame frame = {DS_RELATIONS, batch->data, batch->size};
  return ds_host_frame(collection, remote, &frame);
}

static int admit_cpu(ds_collection *collection, ds_remote *remote, ds_buf *batch,
                     uint64_t user_us, uint64_t system_us) {
  size_t relations_size = batch->size;
  int accepted;
  ds_put_u64(batch, user_us);
  ds_put_u64(batch, system_us);
  assert(!batch->bad);
  ds_frame frame = {DS_RELATIONS_CPU, batch->data, batch->size};
  accepted = ds_host_frame(collection, remote, &frame);
  batch->size = relations_size;
  return accepted;
}

static void check_partial_batch_storage(ds_collection *collection, ds_remote *remote) {
  /* 270^2 and 568^2 give 79 and 131 modulo 10403. Doubling the roots adds
   * the packed factor 2^2. Each new LP label is independent of the 3LP cycle. */
  const unsigned long roots[] = {540, 1136}, labels[] = {79, 131};
  const siqs_factor_t factor = {1, 2};
  siqs_ctx_t *ctx = collection->ctx;
  ds_buf batch = {0};
  siqs_raw_block_t *block = ctx->raw_arena.current;
  mpz_t y;
  unsigned i;
  assert(block != NULL && block->used == 0);
  mpz_init(y);
  ds_put_u32(&batch, DS_RELATION_BATCH_RECORDS);
  for (i = 0; i < DS_RELATION_BATCH_RECORDS; ++i) {
    siqs_raw_relation_t *raw;
    mpz_set_ui(y, roots[i % 2]);
    raw = siqs_raw_relation_new(ctx, y, &factor, 1, 1, labels[i % 2]);
    assert(ds_encode_relation(&batch, raw));
    siqs_raw_relation_free(ctx, raw);
  }
  for (i = 0; i < 3; ++i) {
    assert(admit(collection, remote, &batch));
    assert(ctx->accepted_one_lp == 2);
    assert(ctx->raw_arena.current == block && block->used == 0);
  }
  ds_buf_clear(&batch);
  mpz_clear(y);
}

static void check_cross_worker_combination(void) {
  static const uint32_t primes[] = {2, 3, 5, 7, 11};
  const unsigned long a = 991, b = 2948, invalid[] = {991, 1};
  siqs_ctx_t ctx = {0};
  siqs_factor_array_t result;
  ds_host_state host = {0};
  ds_remote peers[2] = {{0}};
  ds_worker_report reports[2] = {{0}};
  ds_collection_resources resources = {0};
  ds_collection collection = {&ctx, &host, NULL, NULL, &resources};
  ds_buf batch = {0};
  mpz_t n;
  la_col_t *columns;
  unsigned long rows, cols;
  uint64_t mask, *dependencies;
  size_t i;

  mpz_init_set_ui(n, 10403);
  mpz_init_set(ctx.n, n);
  mpz_init_set(ctx.kn, n);
  mpz_inits(ctx.eval.y, ctx.eval.q, ctx.eval.rest, ctx.tlp_upper, NULL);
  ctx.original_n = n;
  ctx.result = &result;
  ctx.params.fb_size = sizeof(primes) / sizeof(*primes);
  ctx.params.max_large_primes = 3;
  ctx.params.large_prime_bound = 1000;
  ctx.largest_fb_prime = 11;
  ctx.fb = siqs_calloc(ctx.params.fb_size, sizeof(*ctx.fb));
  for (i = 0; i < ctx.params.fb_size; ++i) ctx.fb[i].p = primes[i];
  ctx.factor_row_alloc = ctx.params.fb_size + 1;
  ctx.factor_counts = siqs_calloc(ctx.factor_row_alloc, sizeof(*ctx.factor_counts));
  ctx.factor_touched = siqs_calloc(ctx.factor_row_alloc, sizeof(*ctx.factor_touched));
  siqs_factor_array_init(&result, n);
  siqs_hashset_init(&ctx.relation_hashes, 64);
  siqs_lp_combiner_init(&ctx);
  for (i = 0; i < 2; ++i) {
    peers[i].link = DS_LINK_READY;
    peers[i].claim_features = DS_CLAIM_CPU_CHECKPOINTS | DS_CLAIM_PEAK_RSS;
    peers[i].report = reports + i;
    reports[i].jobs = 1;
  }

  encode_triple(&ctx, &batch, invalid, 2);
  assert(!admit_cpu(&collection, peers, &batch, 15, 4));
  assert(ctx.accepted_three_lp == 0 && reports[0].received == 0);
  assert(ctx.raw_arena.current->used == 0);
  assert(resources.received == 0 && resources.wire_bytes == 0);
  assert(reports[0].cpu.user_us == 0 && reports[0].checkpointed_jobs == 0);
  encode_triple(&ctx, &batch, &a, 1);
  assert(admit_cpu(&collection, peers, &batch, 8, 3));
  assert(reports[0].cpu.user_us == 8 && reports[0].cpu.system_us == 3);
  assert(reports[0].checkpointed_jobs == 1);
  assert(ctx.accepted_three_lp == 1 && siqs_relation_count(&ctx) == 0);
  assert(admit(&collection, peers + 1, &batch));
  assert(reports[1].received == 1 && reports[1].accepted == 0);
  encode_triple(&ctx, &batch, &b, 1);
  assert(admit(&collection, peers + 1, &batch));
  assert(ctx.accepted_three_lp == 2 && siqs_relation_count(&ctx) == 1);
  assert(reports[0].accepted == 1 && reports[1].accepted == 1);
  assert(resources.received == 3 && resources.wire_bytes == reports[0].bytes + reports[1].bytes);
  check_partial_batch_storage(&collection, peers);
  batch.size = 0;
  ds_put_u64(&batch, 13);
  ds_put_u64(&batch, 4);
  ds_put_u64(&batch, 3U * 1024U * 1024U);
  assert(ds_host_record_ended(peers, &(ds_frame){DS_ENDED, batch.data, batch.size}));
  assert(reports[0].cpu.user_us == 13 && reports[0].cpu.system_us == 4);
  assert(reports[0].timed_jobs == 1 && reports[0].checkpointed_jobs == 0);
  assert(reports[0].peak_rss_bytes == 3U * 1024U * 1024U);

  columns = siqs_lp_combiner_matrix(&ctx, &rows, &cols);
  assert(rows == 6 && cols == 1 && columns[0].weight == 0);
  dependencies = la_dense_nullspace(rows, cols, columns, &mask);
  assert(dependencies != NULL && mask != 0);
  assert(siqs_test_dependencies(&ctx, columns, cols, dependencies, mask));
  assert(result.count == 2 && siqs_all_factors_prime(&result));
  siqs_verify_partition(n, &result);
  free(columns[0].data);
  free(columns);
  free(dependencies);
  ds_buf_clear(&batch);
  ds_worker_clear(&ctx, &result);
  mpz_clear(n);
  puts("PASS distributed collector: atomic frame rejection, duplicate attribution and exact cross-worker 3LP dependency");
}

static void check_solve_retry(void) {
  static const char input[] =
    "1148829108227602429702806949260709092933481257528975829355909861594747665058507777";
  const char *endpoints[] = {"socketpair-one", "socketpair-two"};
  ds_host host = {.endpoints = endpoints, .count = 2};
  ds_worker_listener workers[2] = {{0}};
  pthread_t sessions[2];
  retry_fixture fixture = {ds_host_get(&host), 0, 0, 0};
  mpz_t n, product;
  mpz_t *values;
  uint32_t count, i;

  for (i = 0; i < 2; ++i) {
    int fd[2];
    ds_worker_session *session = siqs_malloc(sizeof(*session));
    ds_remote *remote = fixture.host->remotes[i];
    ds_buf claim = { &remote->claim_features, 1, 1, 0 };
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
    workers[i].threads = 2;
    workers[i].name = endpoints[i];
    assert(uname(&workers[i].system) == 0);
    assert(pthread_mutex_init(&workers[i].mutex, NULL) == 0);
    session->server = workers + i;
    session->peer = ds_peer_open(fd[0]);
    remote->peer = ds_peer_open(fd[1]);
    assert(session->peer != NULL && remote->peer != NULL);
    remote->link = DS_LINK_CLAIM;
    remote->tried = 1;
    remote->deadline = ds_now() + 10;
    assert(ds_peer_send(remote->peer, DS_CLAIM, &claim));
    assert(pthread_create(sessions + i, NULL, ds_worker_session_run, session) == 0);
  }
  assert(mpz_init_set_str(n, input, 10) == 0);
  retry_test = &fixture;
  values = dmsiqs_factor(&host, n, &count, 2, 0, 2, SIQS_OPTION_3LP, NULL);
  retry_test = NULL;
  assert(!host.failed && count == 2);
  assert(fixture.failures == 2 && fixture.before != 0 && fixture.after > fixture.before);
  for (i = 0; i < 2; ++i)
    assert(fixture.host->remotes[i]->report->polynomials > 0);
  mpz_init_set_ui(product, 1);
  for (i = 0; i < count; ++i) {
    assert(siqs_is_prob_prime(values[i]));
    mpz_mul(product, product, values[i]);
  }
  assert(mpz_cmp(n, product) == 0);
  gmp_siqs_free(values, count);
  dmsiqs_close(&host, NULL, 0);
  for (i = 0; i < 2; ++i) {
    assert(pthread_join(sessions[i], NULL) == 0);
    assert(pthread_mutex_destroy(&workers[i].mutex) == 0);
  }
  mpz_clears(n, product, NULL);
  puts("PASS distributed collector: two workers drain, resume with fresh families after a failed solve, and factor the input");
}

static void check_progress_reporting(void) {
  static const char expected[] =
    "#   renamed sieving: 1.5 polys/s, 0.8 rels/s, last progress 1 s ago\n"
    "#   original disconnected: 0.5 polys/s, 0.3 rels/s\n"
    "#   idle:8012 ready\n"
    "#   pending:8012 waiting\n"
    "#   departed disconnected: 1.0 polys/s, 0.5 rels/s, last progress 12 s ago\n"
    "#   renamed sieving: 3.0 polys/s, 1.5 rels/s\n"
    "#   idle:8012 ready\n"
    "#   pending:8012 waiting\n"
    "#   renamed (active:8012) sieving: 1.5 polys/s, 0.5 rels/s, last progress 4 s ago\n"
    "#   no journal checkpoint yet\n"
    "#   renamed setting up for 55.000s\n"
    "#   renamed (active:8012) sieving: 2.0 polys/s, 1.0 rels/s\n"
    "#   renamed sieving: 1.0 polys/s, 0.5 rels/s, last progress 8 s ago\n"
    "#   renamed disconnected: 0.0 polys/s, 0.0 rels/s, last progress 68 s ago\n";
  ds_worker_report old = {.name = "original", .polynomials = 300, .accepted = 80};
  ds_worker_report recent = {.next = &old, .name = "renamed", .polynomials = 90, .accepted = 48};
  ds_worker_report departed = {.name = "departed", .polynomials = 500, .accepted = 200};
  ds_remote active = {.endpoint = "active:8012", .link = DS_LINK_READY, .configured = 1,
                      .outstanding = 1, .reports = &old, .report = &old};
  ds_remote idle = {.endpoint = "idle:8012", .link = DS_LINK_READY, .listed = 1};
  ds_remote pending = {.endpoint = "pending:8012", .configured = 1};
  ds_remote gone = {.endpoint = "gone:8012", .tried = 1, .listed = 1, .reports = &departed};
  ds_remote *remotes[] = {&active, &idle, &pending, &gone};
  ds_host_state state = {.remotes = remotes, .count = sizeof(remotes) / sizeof(*remotes)};
  siqs_journal_t empty_journal = {0};
  char output[sizeof(expected) + 256];
  size_t length;
  FILE *capture = tmpfile();
  int saved_stdout = dup(STDOUT_FILENO);
  assert(capture != NULL && saved_stdout != -1 && fflush(stdout) == 0);
  assert(dup2(fileno(capture), STDOUT_FILENO) == STDOUT_FILENO);

  ds_host_sample_progress(&state, 100.0, 0, 2);
  old.polynomials += 30;
  old.accepted += 18;
  active.reports = active.report = &recent;
  active.last_progress = 159.0;
  gone.listed = 0;
  gone.last_progress = 148.0;
  departed.polynomials += 60;
  departed.accepted += 30;
  ds_host_sample_progress(&state, 160.0, 60.0, 2);

  /* A new collection phase starts with a fresh baseline after the solve. */
  recent.polynomials += 720;
  recent.accepted += 180;
  ds_host_sample_progress(&state, 1000.0, 0, 2);
  recent.polynomials += 180;
  recent.accepted += 90;
  active.last_progress = 1059.05;
  ds_host_sample_progress(&state, 1060.0, 60.0, 2);

  active.configured = pending.configured = 0;
  active.link = idle.link = DS_LINK_DOWN;
  idle.listed = 0;
  ds_host_sample_progress(&state, 1120.0, 60.0, 2);

  ds_host_sample_progress(&state, 2000.0, 0, 3);
  active.link = DS_LINK_READY;
  active.configured = 1;
  recent.polynomials += 450;
  recent.accepted += 150;
  active.last_progress = 2296.0;
  ds_host_sample_progress(&state, 2300.0, 300.0, 3);
  ds_host_checkpoint_report(&empty_journal, 300, 2);

  state.count = 1;
  recent.next = NULL;
  active.link = DS_LINK_SETUP;
  active.setup_started = 2305.0;
  ds_host_sample_progress(&state, 2360.0, 60.0, 2);
  active.link = DS_LINK_READY;
  recent.polynomials += 60;
  recent.accepted += 30;
  active.last_progress = 2419.5;
  ds_host_sample_progress(&state, 2420.0, 60.0, 2);
  recent.polynomials += 120;
  recent.accepted += 60;
  active.last_progress = 2479.5;
  ds_host_sample_progress(&state, 2480.0, 60.0, 3);
  recent.polynomials += 60;
  recent.accepted += 30;
  active.last_progress = 2532.0;
  ds_host_sample_progress(&state, 2540.0, 60.0, 2);
  active.link = DS_LINK_DOWN;
  active.tried = 1;
  ds_host_sample_progress(&state, 2600.0, 60.0, 2);
  assert(fflush(stdout) == 0);
  assert(dup2(saved_stdout, STDOUT_FILENO) == STDOUT_FILENO);
  assert(close(saved_stdout) == 0);
  rewind(capture);
  length = fread(output, 1, sizeof(output) - 1, capture);
  assert(feof(capture) && !ferror(capture) && fclose(capture) == 0);
  output[length] = '\0';
  if (strcmp(output, expected) != 0) fprintf(stderr, "Unexpected worker progress output:\n%s", output);
  assert(strcmp(output, expected) == 0);
  puts("PASS distributed progress: worker changes, compact rows, checkpoint status, and a fresh baseline after solve");
}

static void check_collection_timing(void) {
  ds_collection_timing timing = {.since = 100.25, .phase = DS_PHASE_STARTUP};
  ds_host_change_phase(&timing, DS_PHASE_COLLECTION, 101.75);
  ds_host_change_phase(&timing, DS_PHASE_COLLECTION, 102.75);
  ds_host_change_phase(&timing, DS_PHASE_WAITING, 105.0);
  ds_host_change_phase(&timing, DS_PHASE_COLLECTION, 107.5);
  ds_host_change_phase(&timing, DS_PHASE_WAITING, 109.25);
  ds_host_change_phase(&timing, DS_PHASE_COLLECTION, 110.0);
  ds_host_change_phase(&timing, DS_PHASE_DRAIN, 111.0);
  ds_host_change_phase(&timing, DS_PHASE_COUNT, 111.5);

  assert(timing.us[DS_PHASE_STARTUP] == 1500000U);
  assert(timing.us[DS_PHASE_COLLECTION] == 6000000U);
  assert(timing.us[DS_PHASE_WAITING] == 3250000U);
  assert(timing.us[DS_PHASE_DRAIN] == 500000U);
  assert(timing.phase == DS_PHASE_COUNT);
  puts("PASS distributed timing: startup, collection, repeated worker waits, and drain");
}

int main(int argc, char **argv) {
  check_cross_worker_combination();
  check_progress_reporting();
  check_collection_timing();
  if (argc == 2 && strcmp(argv[1], "--solve-retry") == 0) check_solve_retry();
  else if (argc != 1) return 1;
  return 0;
}
