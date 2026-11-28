/* The connection manager owns collection state and publishes completed
 * originals. Its private pool uses the ordinary pthread sieve and 3LP jobs. */

#include <errno.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define DS_WORKER_MAX_FAMILIES (2U * PSIQS_MAX_THREADS)
#define DS_RELATION_BATCH_RECORDS 512U
#define DS_RELATION_BATCH_TIME 0.050

typedef struct {
  uint64_t id;
  uint32_t limit;
  uint32_t indices[SIQS_MAX_A_PRIMES];
} ds_family;

typedef struct {
  uint64_t id;
  uint32_t limit;
} ds_active_family;

typedef struct {
  ds_cpu_usage cpu_start;
  time_t started_at;
  double started;
  uint64_t families, polynomials, originals;
  int have_cpu;
} ds_worker_job_stats;

typedef struct {
  ds_peer *peer;
  siqs_ctx_t ctx;
  siqs_factor_array_t result;
  psiqs_pool_t *pool;
  ds_family queue[DS_WORKER_MAX_FAMILIES];
  ds_active_family active[PSIQS_MAX_THREADS];
  uint32_t head, queued, assigned, credit, publication;
  uint64_t last_id;
  mpz_t A, DA, min_A, max_A;
  ds_buf batch, record;
  uint32_t batch_count;
  double batch_started;
  int pausing, paused, divisor_sent;
  uint8_t capabilities;
  ds_worker_job_stats stats;
} ds_worker_job;

typedef struct {
  pthread_mutex_t mutex;
  int busy, stopping, verbose;
  uint32_t threads;
  const char *name;
  struct utsname system;
} ds_worker_listener;

typedef struct {
  ds_worker_listener *server;
  ds_peer *peer;
} ds_worker_session;

typedef struct {
  ds_worker_listener *server;
  char *coordinator, *pool, *endpoint;
  char id[128];
} ds_worker_registration;

static int ds_worker_send(ds_peer *peer, unsigned type, const ds_buf *body) {
  if (body != NULL && (body->bad || body->size > DS_FRAME_MAX)) return 0;
  if (ds_peer_send(peer, type, body)) return 1;
  while (ds_peer_alive(peer)) {
    /* Output backpressure parks the manager while the I/O thread stays live.
     * Each sieve thread can hold only its current publication. */
    int empty = ds_peer_flush(peer, 50);
    if (ds_peer_send(peer, type, body)) return 1;
    if (empty) return 0;
  }
  return 0;
}

static int ds_worker_job_cpu(const ds_worker_job_stats *stats, ds_cpu_usage *cpu,
                             uint64_t *peak_rss_bytes) {
  if (!stats->have_cpu || !ds_process_usage(cpu, peak_rss_bytes) ||
      cpu->user_us < stats->cpu_start.user_us || cpu->system_us < stats->cpu_start.system_us)
    return 0;
  cpu->user_us -= stats->cpu_start.user_us;
  cpu->system_us -= stats->cpu_start.system_us;
  return 1;
}

static int ds_worker_flush_relations(ds_worker_job *job) {
  uint32_t count = job->batch_count;
  unsigned type = DS_RELATIONS;
  ds_cpu_usage cpu;
  int sent;
  if (count == 0) return 1;
  job->batch.data[0] = (unsigned char)(count >> 24);
  job->batch.data[1] = (unsigned char)(count >> 16);
  job->batch.data[2] = (unsigned char)(count >> 8);
  job->batch.data[3] = (unsigned char)count;
  if ((job->capabilities & DS_CLAIM_CPU_CHECKPOINTS) &&
      ds_worker_job_cpu(&job->stats, &cpu, NULL)) {
    ds_put_u64(&job->batch, cpu.user_us);
    ds_put_u64(&job->batch, cpu.system_us);
    type = DS_RELATIONS_CPU;
  }
  sent = ds_worker_send(job->peer, type, &job->batch);
  if (sent) job->stats.originals += count;
  job->batch_count = 0;
  job->batch.size = 0;
  return sent;
}

static int ds_worker_emit(siqs_ctx_t *ctx, const siqs_raw_relation_t *relation) {
  ds_worker_job *job = ctx->distributed_data;
  size_t batch_limit = DS_FRAME_MAX;
  if (job->capabilities & DS_CLAIM_CPU_CHECKPOINTS) batch_limit -= DS_CPU_USAGE_BYTES;
  job->record.size = 0;
  if (!ds_encode_relation(&job->record, relation) ||
      job->record.bad || job->record.size > batch_limit - 4U)
    return 0;
  if (job->record.size > batch_limit - job->batch.size &&
      !ds_worker_flush_relations(job)) return 0;
  if (job->batch_count == 0) {
    ds_put_u32(&job->batch, 0);
    job->batch_started = ds_now();
  }
  ds_put_bytes(&job->batch, job->record.data, job->record.size);
  if (job->batch.bad) return 0;
  ++job->batch_count;
  if (job->batch_count == DS_RELATION_BATCH_RECORDS)
    return ds_worker_flush_relations(job);
  return 1;
}

static int ds_worker_done(ds_worker_job *job, uint64_t id, uint32_t count) {
  ds_buf body = {0};
  int sent;
  ds_put_u64(&body, id);
  ds_put_u32(&body, count);
  sent = ds_worker_send(job->peer, DS_DONE, &body);
  if (sent && count != 0) {
    ++job->stats.families;
    job->stats.polynomials += count;
  }
  ds_buf_clear(&body);
  return sent;
}

static int ds_worker_divisor(ds_worker_job *job) {
  uint32_t i;
  if (job->divisor_sent || !job->ctx.factor_found) return 1;
  for (i = 0; i < job->result.count; ++i) {
    mpz_srcptr value = job->result.values[i];
    if (mpz_cmp_ui(value, 1) > 0 && mpz_cmp(value, job->ctx.n) < 0 &&
        mpz_divisible_p(job->ctx.n, value)) {
      ds_buf body = {0};
      int sent;
      if (!ds_worker_flush_relations(job)) return 0;
      ds_put_mpz(&body, value);
      sent = ds_worker_send(job->peer, DS_DIVISOR, &body);
      ds_buf_clear(&body);
      job->divisor_sent = sent;
      return sent;
    }
  }
  return 0;
}

static int ds_worker_enqueue(ds_worker_job *job, ds_reader *reader) {
  siqs_ctx_t *ctx = &job->ctx;
  uint32_t i, previous = 0, q = ctx->params.q_count;
  uint32_t tail = (job->head + job->queued) % job->credit;
  ds_family *family = job->queue + tail;
  if (job->pausing || job->queued + job->assigned == job->credit) return 0;
  family->id = ds_get_u64(reader);
  family->limit = ds_get_u32(reader);
  if (family->id == 0 || family->id <= job->last_id || family->limit == 0 ||
      family->limit > (UINT32_C(1) << (q - 1U))) return 0;
  mpz_set_ui(job->A, 1);
  for (i = 0; i < q; ++i) {
    uint32_t index = ds_get_u32(reader);
    if (index <= previous || index >= ctx->params.fb_size ||
        ctx->fb[index].sqrt_kn == 0) return 0;
    family->indices[i] = index;
    mpz_mul_ui(job->A, job->A, ctx->fb[index].p);
    previous = index;
  }
  if (!ds_reader_done(reader) || mpz_cmp(job->A, job->min_A) < 0 ||
      mpz_cmp(job->A, job->max_A) > 0) return 0;
  if (job->paused) {
    job->paused = 0;
    psiqs_pool_set_publication_interval(job->pool, job->publication);
  }
  job->last_id = family->id;
  ++job->queued;
  return 1;
}

static void ds_worker_dispatch(ds_worker_job *job) {
  psiqs_pool_t *pool = job->pool;
  uint32_t i;
  if (job->pausing || job->paused || job->ctx.factor_found) return;
  for (i = 0; i < pool->count; ++i) {
    psiqs_worker_t *worker = pool->workers + i;
    ds_active_family *active = job->active + i;
    int idle;
    if (!worker->started || (!active->id && !job->queued)) continue;
    pthread_mutex_lock(&pool->mutex);
    idle = worker->state == PSIQS_IDLE && worker->task == PSIQS_TASK_SIEVE;
    pthread_mutex_unlock(&pool->mutex);
    if (!idle) continue;
    if (active->id) {
      psiqs_prepare_publication(worker, active->limit - worker->family_polynomials);
    } else {
      const ds_family *family = job->queue + job->head;
      uint32_t j;
      mpz_set_ui(job->A, 1);
      for (j = 0; j < job->ctx.params.q_count; ++j)
        mpz_mul_ui(job->A, job->A, job->ctx.fb[family->indices[j]].p);
      mpz_mul_ui(job->DA, job->A, job->ctx.params.poly_d);
      psiqs_install_family(worker, family->indices, job->A, job->DA, family->limit);
      active->id = family->id;
      active->limit = family->limit;
      job->head = (job->head + 1U) % job->credit;
      --job->queued;
      ++job->assigned;
    }
    pthread_mutex_lock(&pool->mutex);
    worker->state = PSIQS_WORK;
    pthread_cond_signal(&worker->work);
    pthread_mutex_unlock(&pool->mutex);
  }
}

static int ds_worker_ready(ds_worker_job *job, int *remaining) {
  psiqs_pool_t *pool = job->pool;
  psiqs_worker_t *worker;
  ds_active_family *active;
  int ready;
  pthread_mutex_lock(&pool->mutex);
  ready = pool->queued != 0;
  pthread_mutex_unlock(&pool->mutex);
  *remaining = ready;
  if (!ready) return 1;
  worker = psiqs_pool_take(pool);
  active = job->active + worker->index;
  if (worker->task == PSIQS_TASK_GCD) {
    (void)psiqs_release_gcd_worker(&job->ctx, worker);
  } else {
    pool->merging_worker = worker;
    pool->merging_dispatched = 0;
    psiqs_merge_worker(&job->ctx, worker);
    pool->merging_worker = NULL;
    siqs_tlp_poll(&job->ctx);
  }
  if (job->ctx.distributed_failed || !ds_worker_divisor(job)) return 0;
  if (active->id && (job->ctx.factor_found || job->pausing ||
                     worker->family_polynomials == active->limit)) {
    if (!ds_worker_done(job, active->id, worker->family_polynomials)) return 0;
    active->id = 0;
    --job->assigned;
  }
  return 1;
}

static int ds_worker_pause(ds_worker_job *job) {
  if (job->pausing || job->paused) return 0;
  job->pausing = 1;
  psiqs_pool_set_publication_interval(job->pool, 32);
  while (job->queued) {
    if (!ds_worker_done(job, job->queue[job->head].id, 0)) return 0;
    job->head = (job->head + 1U) % job->credit;
    --job->queued;
  }
  return 1;
}

static int ds_worker_finish_pause(ds_worker_job *job) {
  psiqs_pool_t *pool = job->pool;
  uint32_t i;
  if (!job->pausing) return 1;
  pthread_mutex_lock(&pool->mutex);
  for (i = 0; i < pool->count; ++i) {
    if (pool->workers[i].started && pool->workers[i].state != PSIQS_IDLE) {
      pthread_mutex_unlock(&pool->mutex);
      return 1;
    }
  }
  pthread_mutex_unlock(&pool->mutex);
  /* An earlier publication may already have parked a family when a divisor
   * from a different worker prevented another dispatch. */
  for (i = 0; i < pool->count; ++i) {
    ds_active_family *active = job->active + i;
    if (!active->id) continue;
    if (!ds_worker_done(job, active->id, pool->workers[i].family_polynomials))
      return 0;
    active->id = 0;
    --job->assigned;
  }
  siqs_tlp_flush(&job->ctx);
  if (job->ctx.distributed_failed || !ds_worker_divisor(job) ||
      !ds_worker_flush_relations(job) || !ds_worker_send(job->peer, DS_PAUSED, NULL))
    return 0;
  job->pausing = 0;
  job->paused = 1;
  return 1;
}

static void ds_worker_job_clear(ds_worker_job *job) {
  if (job == NULL) return;
  /* Join assigned GCD jobs before freeing their storage. Ignore pending
   * output during teardown since either side has ended the attempt. */
  job->ctx.factor_found = 1;
  if (job->pool) psiqs_pool_destroy(job->pool);
  ds_worker_clear(&job->ctx, &job->result);
  mpz_clears(job->A, job->DA, job->min_A, job->max_A, NULL);
  ds_buf_clear(&job->batch);
  ds_buf_clear(&job->record);
  free(job);
}

static void ds_worker_timestamp(char stamp[32], time_t when) {
  struct tm utc;
  if (gmtime_r(&when, &utc) == NULL || strftime(stamp, 32, "%Y-%m-%dT%H:%M:%SZ", &utc) == 0)
    strcpy(stamp, "unknown time");
}

static void ds_worker_job_finish(ds_worker_job *job, const char *name, ds_buf *ended) {
  ds_worker_job_stats stats;
  ds_cpu_usage cpu;
  uint64_t peak_rss_bytes = 0;
  double elapsed;
  int report_peak;
  char stamp[32], cpu_text[128] = "CPU time unavailable";
  if (job == NULL) return;
  stats = job->stats;
  report_peak = ended != NULL && (job->capabilities & DS_CLAIM_PEAK_RSS);
  ds_worker_job_clear(job);
  elapsed = ds_now() - stats.started;
  if (ds_worker_job_cpu(&stats, &cpu, report_peak ? &peak_rss_bytes : NULL)) {
    double user, system;
    if (ended != NULL) {
      ds_put_u64(ended, cpu.user_us);
      ds_put_u64(ended, cpu.system_us);
      if (report_peak) ds_put_u64(ended, peak_rss_bytes);
      if (ended->bad) ds_buf_clear(ended);
    }
    user = (double)cpu.user_us / 1e6;
    system = (double)cpu.system_us / 1e6;
    snprintf(cpu_text, sizeof(cpu_text), "%.6f core-hours (%.3f s user, %.3f s system)",
             (user + system) / 3600.0, user, system);
  }
  ds_worker_timestamp(stamp, time(NULL));
  fprintf(stderr, "%s: %s job on worker %s in %.3f s, %llu families and %llu polynomials reported, "
          "%llu originals published, %s\n", stamp, ended ? "Completed" : "Interrupted", name, elapsed,
          (unsigned long long)stats.families, (unsigned long long)stats.polynomials,
          (unsigned long long)stats.originals, cpu_text);
  fflush(stderr);
}

static ds_worker_job *ds_worker_job_open(ds_peer *peer, ds_reader *reader,
                                         uint32_t threads, uint8_t capabilities,
                                         const char **error) {
  ds_worker_job *job = calloc(1, sizeof(*job));
  uint32_t factor, i;
  if (job == NULL) {
    *error = DS_SETUP_RETRY_ERROR;
    return NULL;
  }
  job->stats.started = ds_now();
  job->stats.started_at = time(NULL);
  job->stats.have_cpu = ds_process_cpu(&job->stats.cpu_start);
  job->capabilities = capabilities;
  if (!ds_worker_setup(reader, &job->ctx, &job->result, threads)) {
    *error = "invalid sieve setup";
    free(job);
    return NULL;
  }
  job->peer = peer;
  job->ctx.distributed_emit = ds_worker_emit;
  job->ctx.distributed_data = job;
  mpz_inits(job->A, job->DA, job->min_A, job->max_A, NULL);
  factor = siqs_A_maximum_factor(&job->ctx, job->ctx.params.q_count);
  if (factor == 0 || (job->pool = psiqs_pool_create(&job->ctx)) == NULL) {
    *error = factor == 0 ? "invalid sieve setup" : DS_SETUP_RETRY_ERROR;
    ds_worker_job_clear(job);
    return NULL;
  }
  for (i = 0; !job->pool->workers[i].started; ++i) {}
  mpz_cdiv_q_ui(job->min_A, job->pool->workers[i].poly.target_A, factor);
  mpz_mul_ui(job->max_A, job->pool->workers[i].poly.target_A, factor);
  job->publication = job->pool->publication_polynomials;
  job->credit = job->pool->live * 2U;
  return job;
}

static void ds_worker_identity_field(ds_buf *body, const char *value) {
  size_t length;
  if (value == NULL || !*value) value = "unknown";
  length = strlen(value);
  if (length > 127U) length = 127U;
  ds_put_u8(body, (uint8_t)length);
  ds_put_bytes(body, value, length);
}

static int ds_worker_claim_valid(const ds_frame *frame) {
  return frame->type == DS_CLAIM &&
         (frame->size == 0 ||
          (frame->size == 1 && (frame->data[0] == DS_CLAIM_CPU_CHECKPOINTS ||
                               frame->data[0] == (DS_CLAIM_CPU_CHECKPOINTS | DS_CLAIM_PEAK_RSS))));
}

static int ds_worker_claim(ds_peer *peer, const ds_worker_listener *server,
                           uint8_t *capabilities) {
  ds_frame frame = {0};
  ds_buf identity = {0};
  int okay = ds_peer_recv(peer, &frame, 5000) == 1 && ds_worker_claim_valid(&frame);
  *capabilities = okay && frame.size != 0 ? frame.data[0] : 0;
  ds_frame_clear(&frame);
  if (!okay) return 0;
  ds_worker_identity_field(&identity, server->name ? server->name : server->system.nodename);
  ds_worker_identity_field(&identity, server->system.nodename);
  ds_worker_identity_field(&identity, server->system.sysname);
  ds_worker_identity_field(&identity, server->system.machine);
  ds_put_u32(&identity, server->threads);
  okay = ds_worker_send(peer, DS_GRANTED, &identity);
  ds_buf_clear(&identity);
  return okay;
}

static void *ds_worker_session_run(void *arg) {
  ds_worker_session *session = arg;
  ds_worker_listener *server = session->server;
  ds_peer *peer = session->peer;
  ds_worker_job *job = NULL;
  uint8_t capabilities;
  const char *error = NULL;
  const char *name = server->name ? server->name : server->system.nodename;
  if (!*name) name = "unknown";
  free(session);
  if (ds_worker_claim(peer, server, &capabilities)) {
    while (ds_peer_alive(peer)) {
      ds_frame frame = {0};
      int ready = 0, status;
      if (job) {
        if (!ds_worker_ready(job, &ready) || !ds_worker_finish_pause(job)) break;
        if (job->batch_count && ds_now() - job->batch_started >= DS_RELATION_BATCH_TIME &&
            !ds_worker_flush_relations(job)) break;
        ds_worker_dispatch(job);
      }
      status = ds_peer_recv(peer, &frame, ready ? 0 : job ? 2 : 1000);
      if (status < 0) break;
      if (status == 0) continue;
      if (frame.type == DS_SETUP && job == NULL) {
        ds_reader reader = { frame.data, frame.size, 0 };
        job = ds_worker_job_open(peer, &reader, server->threads, capabilities, &error);
        if (job != NULL) {
          ds_buf body = {0};
          char stamp[32], setup[32];
          ds_worker_timestamp(stamp, job->stats.started_at);
          siqs_log_duration(setup, sizeof(setup), ds_now() - job->stats.started);
          gmp_fprintf(stderr, "%s: Started job on worker %s for %Zd (%u bits, %u-LP, %u sieve threads, setup %s)\n",
                      stamp, name, job->ctx.n, job->ctx.params.bits,
                      job->ctx.params.max_large_primes, job->pool->live, setup);
          if (server->verbose > 2) {
            if (job->ctx.block_prime_count != 0)
              fprintf(stderr, "%s: Worker %s sieve block %u bytes (max %u)\n", stamp, name,
                      job->ctx.block_length, (unsigned)SIQS_SIEVE_BLOCK_SIZE);
            else
              fprintf(stderr, "%s: Worker %s sieve blocking disabled\n", stamp, name);
          }
          fflush(stderr);
          ds_put_u32(&body, job->credit);
          if (!ds_worker_send(peer, DS_READY, &body)) error = "could not publish worker capacity";
          ds_buf_clear(&body);
        }
      } else if (frame.type == DS_FAMILY && job) {
        ds_reader reader = { frame.data, frame.size, 0 };
        if (!ds_worker_enqueue(job, &reader)) error = "invalid family assignment";
      } else if (frame.type == DS_PAUSE && frame.size == 0 && job) {
        if (!ds_worker_pause(job)) error = "invalid pause";
      } else if (frame.type == DS_END && frame.size == 0 && job) {
        ds_buf body = {0};
        ds_worker_job_finish(job, name, &body);
        job = NULL;
        if (!ds_worker_send(peer, DS_ENDED, &body)) error = "could not confirm attempt end";
        ds_buf_clear(&body);
      } else error = "unexpected worker command";
      ds_frame_clear(&frame);
      if (error) break;
    }
  } else error = "expected worker claim";
  if (error && ds_peer_alive(peer)) {
    ds_buf body = {0};
    ds_put_bytes(&body, error, strlen(error));
    if (ds_peer_send(peer, DS_ERROR, &body)) (void)ds_peer_flush(peer, 250);
    ds_buf_clear(&body);
  }
  ds_worker_job_finish(job, name, NULL);
  ds_peer_close(peer);
  pthread_mutex_lock(&server->mutex);
  server->busy = 0;
  pthread_mutex_unlock(&server->mutex);
  return NULL;
}

static void ds_worker_registration_free(ds_worker_registration *registration) {
  free(registration->coordinator);
  free(registration->pool);
  free(registration->endpoint);
  free(registration);
}

static void *ds_worker_register_run(void *arg) {
  ds_worker_registration *registration = arg;
  ds_worker_listener *server = registration->server;
  const struct timespec interval = {3, 0};
  int previous = -1;
  for (;;) {
    int registered, stopping;
    pthread_mutex_lock(&server->mutex);
    stopping = server->stopping;
    pthread_mutex_unlock(&server->mutex);
    if (stopping) break;
    registered = ds_registry_register(registration->coordinator, registration->pool,
                                      registration->id, registration->endpoint);
    if (registered != previous) {
      if (registered)
        fprintf(stderr, "dmsiqs: worker %s registered at %s in pool %s\n",
                registration->id, registration->endpoint, registration->pool);
      else
        fprintf(stderr, "dmsiqs: worker %s could not register with %s, retrying\n",
                registration->id, registration->coordinator);
      previous = registered;
    }
    (void)nanosleep(&interval, NULL);
  }
  ds_worker_registration_free(registration);
  return NULL;
}

static int ds_worker_start_registration(ds_worker_listener *server,
                                        const pthread_attr_t *detached,
                                        const char *port, const char *coordinator,
                                        const char *pool, const char *endpoint) {
  ds_worker_registration *registration;
  pthread_t thread;
  if (!pool || !endpoint) return 0;
  registration = calloc(1, sizeof(*registration));
  if (!registration) return 0;
  registration->server = server;
  registration->coordinator = strdup(coordinator);
  registration->pool = strdup(pool);
  registration->endpoint = strdup(endpoint);
  if (server->name)
    snprintf(registration->id, sizeof(registration->id), "%s", server->name);
  else
    snprintf(registration->id, sizeof(registration->id), "%.121s:%u",
             server->system.nodename[0] ? server->system.nodename : "worker",
             (unsigned)strtoul(port, NULL, 10));
  if (registration->coordinator && registration->pool && registration->endpoint &&
      pthread_create(&thread, detached, ds_worker_register_run, registration) == 0)
    return 1;
  ds_worker_registration_free(registration);
  return 0;
}

static int ds_worker_server(const char *bind_address, const char *port,
                            uint32_t threads, const char *name,
                            const char *coordinator, const char *pool,
                            const char *advertise, int verbose) {
  ds_worker_listener *server = calloc(1, sizeof(*server));
  pthread_attr_t detached;
  int listener, attributes_ready, mutex_ready;
  if (server == NULL) return 1;
  listener = ds_listen(bind_address, port);
  if (listener < 0) {
    fprintf(stderr, "dmsiqs: could not listen on %s:%s\n", bind_address, port);
    free(server);
    return 1;
  }
  server->threads = threads;
  server->name = name;
  server->verbose = verbose;
  if (uname(&server->system) != 0) memset(&server->system, 0, sizeof(server->system));
  mutex_ready = pthread_mutex_init(&server->mutex, NULL) == 0;
  attributes_ready = mutex_ready && pthread_attr_init(&detached) == 0;
  if (!attributes_ready ||
      pthread_attr_setdetachstate(&detached, PTHREAD_CREATE_DETACHED) != 0) {
    fprintf(stderr, "dmsiqs: could not initialize worker manager\n");
    close(listener);
    if (attributes_ready) pthread_attr_destroy(&detached);
    if (mutex_ready) pthread_mutex_destroy(&server->mutex);
    free(server);
    return 1;
  }
  fprintf(stderr, "dmsiqs: worker %s listening on %s:%s with %u sieve threads\n",
          name ? name : server->system.nodename, bind_address, port, threads);
  if (coordinator && !ds_worker_start_registration(server, &detached, port,
                                                   coordinator, pool, advertise)) {
    fprintf(stderr, "dmsiqs: could not start worker registration\n");
    close(listener);
    pthread_attr_destroy(&detached);
    pthread_mutex_destroy(&server->mutex);
    free(server);
    return 1;
  }
  for (;;) {
    int busy, client = accept(listener, NULL, NULL);
    ds_peer *peer;
    ds_worker_session *session;
    pthread_t manager;
    if (client < 0) {
      if (errno == EINTR || errno == ECONNABORTED) continue;
      perror("dmsiqs: accept");
      break;
    }
    peer = ds_peer_open(client);
    if (peer == NULL) continue;
    pthread_mutex_lock(&server->mutex);
    busy = server->busy;
    if (!busy) server->busy = 1;
    pthread_mutex_unlock(&server->mutex);
    if (busy) {
      ds_frame claim = {0};
      if (ds_peer_recv(peer, &claim, 1000) == 1 &&
          ds_worker_claim_valid(&claim) &&
          ds_peer_send(peer, DS_BUSY, NULL))
        (void)ds_peer_flush(peer, 250);
      ds_frame_clear(&claim);
      ds_peer_close(peer);
      continue;
    }
    session = malloc(sizeof(*session));
    if (session) {
      session->peer = peer;
      session->server = server;
      if (pthread_create(&manager, &detached, ds_worker_session_run, session) == 0)
        continue;
      free(session);
    }
    ds_peer_close(peer);
    pthread_mutex_lock(&server->mutex);
    server->busy = 0;
    pthread_mutex_unlock(&server->mutex);
  }
  pthread_mutex_lock(&server->mutex);
  server->stopping = 1;
  pthread_mutex_unlock(&server->mutex);
  close(listener);
  pthread_attr_destroy(&detached);
  /* The command exits after a listener failure. Detached managers and the
   * registration thread can still reference this state until process exit. */
  return 1;
}
