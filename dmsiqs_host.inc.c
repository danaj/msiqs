/* The factoring thread owns the collector and the assignment ledger. Peer
 * threads do socket I/O only. A short-lived connector keeps a dead endpoint
 * from blocking relation ingestion from working machines. */
#include <time.h>
#include <unistd.h>

#define DS_STALL_RETRY_SECONDS 15.0
#define DS_SETUP_RETRY_LIMIT 2U
#define DS_SETUP_MAX_BACKOFF_EXPONENT 4U

enum ds_host_link {
  DS_LINK_DOWN, DS_LINK_CLAIM, DS_LINK_LEASE, DS_LINK_SETUP,
  DS_LINK_READY, DS_LINK_PAUSING, DS_LINK_PAUSED, DS_LINK_ENDING
};

typedef struct {
  uint64_t id;
  uint32_t limit;
} ds_ticket;

typedef struct ds_worker_report {
  struct ds_worker_report *next;
  char name[128], hostname[128], system[128], machine[128];
  uint32_t threads;
  uint64_t families, polynomials, received, accepted, bytes;
  uint64_t polynomials_at_report, originals_at_report;
  uint64_t jobs, timed_jobs, checkpointed_jobs;
  uint64_t peak_rss_bytes;
  size_t input_peak_bytes;
  ds_cpu_usage cpu;
} ds_worker_report;

typedef struct {
  char *endpoint;
  ds_peer *peer;
  enum ds_host_link link;
  double retry, deadline, progress_deadline, stall_retry;
  double last_progress, setup_started;
  int configured, listed, seen;
  int busy, rejected, tried, busy_announced;
  int stall_pending;
  unsigned char claim_features;
  unsigned setup_failures;
  int have_checkpoint;
  ds_cpu_usage job_cpu;
  pthread_mutex_t connect_mutex;
  pthread_t connector;
  int connecting, connect_done, connected_fd;
  uint32_t capacity, outstanding;
  ds_ticket *tickets;
  ds_worker_report *reports, *report;
} ds_remote;

typedef struct {
  const char *endpoint, *pool;
  pthread_t thread;
  pthread_mutex_t mutex;
  char **entries;
  size_t count;
  ds_remote *stall_remote;
  int stall_answered;
  double next_poll;
  int running, done, success, unavailable;
} ds_host_registry;

typedef struct {
  ds_remote **remotes;
  size_t count, capacity, first;
  ds_host_registry registry;
  uint64_t next_id, reserved, uncertain;
  uint32_t worker_stall_seconds, worker_report_seconds;
  int ever_ready, targets_announced;
} ds_host_state;

typedef struct {
  uint64_t received, wire_bytes;
  double import_seconds;
} ds_collection_resources;

typedef struct {
  siqs_ctx_t *ctx;
  ds_host_state *state;
  uint64_t *families, *polynomials;
  ds_collection_resources *resources;
} ds_collection;

enum ds_collection_phase {
  DS_PHASE_STARTUP, DS_PHASE_COLLECTION, DS_PHASE_WAITING, DS_PHASE_DRAIN, DS_PHASE_COUNT
};

typedef struct {
  double since;
  enum ds_collection_phase phase;
  uint64_t us[DS_PHASE_COUNT];
} ds_collection_timing;

static void ds_host_change_phase(ds_collection_timing *timing, enum ds_collection_phase phase,
                                  double now) {
  if (phase == timing->phase) return;
  timing->us[timing->phase] += (uint64_t)((now - timing->since) * 1e6 + 0.5);
  timing->phase = phase;
  timing->since = now;
}

static void ds_host_idle(void) {
  const struct timespec sleep_time = {0, 10000000L};
  (void)nanosleep(&sleep_time, NULL);
}

/* The pointer array can grow while connectors still use their own records. */
static ds_remote *ds_host_add_remote(ds_host_state *state, const char *endpoint) {
  ds_remote *remote;
  size_t i, size;
  for (i = 0; i < state->count; ++i)
    if (strcmp(state->remotes[i]->endpoint, endpoint) == 0) return state->remotes[i];
  if (state->count == state->capacity) {
    size_t capacity = state->capacity ? siqs_array_size(state->capacity, 2) : 8;
    state->remotes = siqs_realloc(state->remotes, siqs_array_size(capacity, sizeof(*state->remotes)));
    state->capacity = capacity;
  }
  remote = siqs_calloc(1, sizeof(*remote));
  size = strlen(endpoint) + 1;
  remote->endpoint = siqs_malloc(size);
  memcpy(remote->endpoint, endpoint, size);
  remote->connected_fd = -1;
  remote->claim_features = DS_CLAIM_CPU_CHECKPOINTS | DS_CLAIM_PEAK_RSS;
  if (pthread_mutex_init(&remote->connect_mutex, NULL) != 0)
    croak("DMSIQS: cannot initialize connector mutex");
  state->remotes[state->count++] = remote;
  return remote;
}

static ds_host_state *ds_host_get(ds_host *host) {
  ds_host_state *state = host->state;
  size_t i;
  if (state != NULL) return state;
  state = siqs_calloc(1, sizeof(*state));
  state->worker_stall_seconds = host->worker_stall_seconds;
  state->worker_report_seconds = host->worker_report_seconds;
  for (i = 0; i < host->count; ++i)
    ds_host_add_remote(state, host->endpoints[i])->configured = 1;
  state->registry.endpoint = host->coordinator;
  state->registry.pool = host->pool != NULL ? host->pool : DS_DEFAULT_POOL;
  if (state->registry.endpoint != NULL && pthread_mutex_init(&state->registry.mutex, NULL) != 0)
    croak("DMSIQS: cannot initialize coordinator mutex");
  host->state = state;
  return state;
}

static void *ds_host_registry_main(void *arg) {
  ds_host_registry *registry = arg;
  char **entries = NULL;
  size_t count = 0;
  int success;
  /* The factoring thread keeps the selected endpoint alive until the join. */
  if (registry->stall_remote != NULL)
    registry->stall_answered = ds_registry_report_stall(registry->endpoint, registry->pool,
                                                       registry->stall_remote->endpoint);
  success = ds_registry_list(registry->endpoint, registry->pool, &entries, &count);
  pthread_mutex_lock(&registry->mutex);
  registry->entries = entries;
  registry->count = count;
  registry->success = success;
  registry->done = 1;
  pthread_mutex_unlock(&registry->mutex);
  return NULL;
}

static int ds_host_poll_registry(ds_host_state *state, int verbose) {
  ds_host_registry *registry = &state->registry;
  size_t i, kept;
  int done;
  double now;
  if (registry->endpoint == NULL) return 0;
  now = ds_now();
  if (!registry->running) {
    if (now < registry->next_poll) return 0;
    registry->stall_remote = NULL;
    registry->stall_answered = 0;
    for (i = 0; i < state->count; ++i) {
      if (state->remotes[i]->stall_pending) {
        registry->stall_remote = state->remotes[i];
        break;
      }
    }
    registry->done = 0;
    if (pthread_create(&registry->thread, NULL, ds_host_registry_main, registry) == 0) {
      registry->running = 1;
      return 1;
    }
    registry->success = 0;
  } else {
    pthread_mutex_lock(&registry->mutex);
    done = registry->done;
    pthread_mutex_unlock(&registry->mutex);
    if (!done) return 0;
    pthread_join(registry->thread, NULL);
    registry->running = 0;
    if (registry->stall_answered) registry->stall_remote->stall_pending = 0;
    registry->stall_remote = NULL;
  }
  registry->next_poll = now + 2.0;
  if (registry->success) {
    for (i = 0; i < registry->count; ++i) {
      ds_remote *remote = ds_host_add_remote(state, registry->entries[i]);
      if (!remote->listed) {
        remote->rejected = 0;
        remote->retry = 0;
        remote->setup_failures = 0;
      }
      remote->seen = 1;
    }
    if (registry->unavailable && verbose > 0) {
      printf("# dmsiqs coordinator %s reachable\n", registry->endpoint);
      fflush(stdout);
    }
    registry->unavailable = 0;
  } else if (!registry->unavailable) {
    fprintf(stderr, "DMSIQS: coordinator %s unavailable, retrying\n", registry->endpoint);
    registry->unavailable = 1;
  }
  /* The latest snapshot permits fresh dials. Existing leases and their
   * accounting survive even if discovery expires or the registry is down. */
  for (i = 0, kept = 0; i < state->count; ++i) {
    ds_remote *remote = state->remotes[i];
    remote->listed = remote->seen;
    remote->seen = 0;
    if (remote->configured || remote->listed || remote->peer != NULL ||
        remote->connecting || remote->stall_pending || remote->reports != NULL) {
      state->remotes[kept++] = remote;
    } else {
      pthread_mutex_destroy(&remote->connect_mutex);
      free(remote->endpoint);
      free(remote);
    }
  }
  state->count = kept;
  state->first = kept ? state->first % kept : 0;
  ds_registry_free(registry->entries, registry->count);
  registry->entries = NULL;
  registry->count = 0;
  return 1;
}

static void *ds_host_connect_main(void *arg) {
  ds_remote *remote = arg;
  int fd = ds_connect(remote->endpoint, 1000);
  pthread_mutex_lock(&remote->connect_mutex);
  remote->connected_fd = fd;
  remote->connect_done = 1;
  pthread_mutex_unlock(&remote->connect_mutex);
  return NULL;
}

static void ds_host_close_peer(ds_remote *remote) {
  if (remote->peer != NULL) {
    if (remote->report != NULL) {
      size_t peak = ds_peer_input_high_water(remote->peer);
      if (peak > remote->report->input_peak_bytes) remote->report->input_peak_bytes = peak;
    }
    ds_peer_close(remote->peer);
  }
  remote->peer = NULL;
  remote->link = DS_LINK_DOWN;
  remote->claim_features = DS_CLAIM_CPU_CHECKPOINTS | DS_CLAIM_PEAK_RSS;
  remote->retry = ds_now() + 2.0;
  remote->deadline = 0;
  remote->progress_deadline = 0;
  free(remote->tickets);
  remote->tickets = NULL;
  remote->capacity = 0;
  remote->outstanding = 0;
  remote->report = NULL;
}

static const char *ds_host_print_name(char output[128], const char *name) {
  size_t i;
  for (i = 0; name[i] != '\0'; ++i)
    output[i] = name[i] >= 0x20 && (unsigned char)name[i] <= 0x7e ? name[i] : '?';
  output[i] = '\0';
  return output;
}

static void ds_host_drop(ds_collection *collection, ds_remote *remote,
                         const char *reason, int rejected) {
  char name[128];
  uint32_t i;
  for (i = 0; i < remote->capacity; ++i) {
    uint32_t limit = remote->tickets[i].limit;
    if (remote->tickets[i].id != 0) {
      collection->state->reserved -= limit;
      /* The host cannot know how much of this assignment ran. Spend the
       * full reservation and retain all complete frames already imported. */
      *collection->polynomials += limit;
      collection->state->uncertain += limit;
    }
  }
  if (remote->report != NULL)
    fprintf(stderr, "DMSIQS: worker %s (%s) %s\n",
            ds_host_print_name(name, remote->report->name), remote->endpoint, reason);
  else
    fprintf(stderr, "DMSIQS: worker %s %s\n", remote->endpoint, reason);
  remote->busy = 0;
  remote->rejected = rejected;
  ds_host_close_peer(remote);
}

static int ds_host_dial(ds_remote *remote, double now) {
  int done, fd;
  if (remote->connecting) {
    pthread_mutex_lock(&remote->connect_mutex);
    done = remote->connect_done;
    fd = remote->connected_fd;
    pthread_mutex_unlock(&remote->connect_mutex);
    if (!done) return 0;
    pthread_join(remote->connector, NULL);
    remote->connecting = 0;
    remote->connected_fd = -1;
    remote->tried = 1;
    remote->retry = now + 2.0;
    remote->busy = 0;
    if (fd >= 0) {
      if (remote->configured || remote->listed) remote->peer = ds_peer_open(fd);
      else close(fd);
    }
    if (remote->peer != NULL) {
      ds_buf claim = { &remote->claim_features, 1, 1, 0 };
      if (ds_peer_send(remote->peer, DS_CLAIM, remote->claim_features ? &claim : NULL)) {
        remote->link = DS_LINK_CLAIM;
        remote->deadline = now + 10.0;
      } else ds_host_close_peer(remote);
    }
    return 1;
  }
  if ((!remote->configured && !remote->listed) || remote->rejected ||
      now < remote->retry || now < remote->stall_retry) return 0;
  remote->connect_done = 0;
  if (pthread_create(&remote->connector, NULL, ds_host_connect_main, remote) != 0) {
    remote->tried = 1;
    remote->retry = now + 2.0;
    fprintf(stderr, "DMSIQS: unable to start connector for %s\n", remote->endpoint);
  } else remote->connecting = 1;
  return 1;
}

static int ds_host_get_name(ds_reader *r, char name[128]) {
  unsigned size = ds_get_u8(r), i;
  const unsigned char *data;
  if (size == 0 || size > 127) return 0;
  data = ds_get_bytes(r, size);
  if (data == NULL) return 0;
  for (i = 0; i < size; ++i) {
    if (data[i] == 0) return 0;
    name[i] = (char)data[i];
  }
  name[size] = '\0';
  return 1;
}

static int ds_host_identify(ds_remote *remote, ds_reader *r, int verbose) {
  ds_worker_report identity = {0}, *report;
  if (!ds_host_get_name(r, identity.name) || !ds_host_get_name(r, identity.hostname) ||
      !ds_host_get_name(r, identity.system) || !ds_host_get_name(r, identity.machine)) return 0;
  identity.threads = ds_get_u32(r);
  if (!ds_reader_done(r) || identity.threads == 0 || identity.threads > PSIQS_MAX_THREADS) return 0;
  for (report = remote->reports; report != NULL; report = report->next) {
    if (strcmp(report->name, identity.name) == 0 &&
        strcmp(report->hostname, identity.hostname) == 0 &&
        strcmp(report->system, identity.system) == 0 &&
        strcmp(report->machine, identity.machine) == 0 &&
        report->threads == identity.threads) break;
  }
  if (report == NULL) {
    report = siqs_malloc(sizeof(*report));
    *report = identity;
    report->next = remote->reports;
    remote->reports = report;
  }
  remote->report = report;
  if (verbose > 2) {
    char name[128];
    printf("# dmsiqs worker %s (%s) claimed, %u configured threads\n",
           ds_host_print_name(name, report->name), remote->endpoint, report->threads);
    fflush(stdout);
  }
  return 1;
}

static uint64_t ds_host_accepted(const siqs_ctx_t *ctx) {
  return ctx->accepted_smooth + ctx->accepted_one_lp + ctx->accepted_two_lp + ctx->accepted_three_lp;
}

static int ds_host_read_cpu(const ds_remote *remote, ds_reader *reader, ds_cpu_usage *cpu) {
  ds_cpu_usage last = remote->job_cpu, total = remote->report->cpu;
  cpu->user_us = ds_get_u64(reader);
  cpu->system_us = ds_get_u64(reader);
  return ds_reader_done(reader) && cpu->user_us >= last.user_us &&
         cpu->system_us >= last.system_us &&
         cpu->user_us - last.user_us <= UINT64_MAX - total.user_us &&
         cpu->system_us - last.system_us <= UINT64_MAX - total.system_us;
}

static void ds_host_add_cpu(ds_remote *remote, ds_cpu_usage cpu) {
  ds_worker_report *report = remote->report;
  report->cpu.user_us += cpu.user_us - remote->job_cpu.user_us;
  report->cpu.system_us += cpu.system_us - remote->job_cpu.system_us;
  remote->job_cpu = cpu;
}

static void ds_host_record_batch_cpu(ds_remote *remote, ds_cpu_usage cpu) {
  ds_host_add_cpu(remote, cpu);
  if (!remote->have_checkpoint) {
    remote->have_checkpoint = 1;
    ++remote->report->checkpointed_jobs;
  }
}

static void ds_host_note_progress(const ds_host_state *state, ds_remote *remote,
                                  int did_work) {
  double now = ds_now();
  if (did_work) remote->last_progress = now;
  remote->progress_deadline = state->worker_stall_seconds != 0 && remote->outstanding != 0
      ? now + state->worker_stall_seconds : 0;
}

/* A new connection may reach a replacement with fewer claim capabilities. */
static int ds_host_retry_claim(ds_remote *remote) {
  unsigned char features = remote->claim_features;
  if (remote->link != DS_LINK_CLAIM || features == 0) return 0;
  ds_host_close_peer(remote);
  remote->claim_features = features & DS_CLAIM_PEAK_RSS ? DS_CLAIM_CPU_CHECKPOINTS : 0;
  remote->tried = 0;
  remote->retry = 0;
  return 1;
}

static int ds_host_frame(ds_collection *collection, ds_remote *remote,
                         const ds_frame *frame) {
  ds_host_state *state = collection->state;
  siqs_ctx_t *ctx = collection->ctx;
  ds_reader r = {frame->data, frame->size, 0};
  if (frame->type == DS_ERROR) {
    static const char claim_error[] = "expected worker claim";
    if (frame->size == sizeof(claim_error) - 1 &&
        memcmp(frame->data, claim_error, sizeof(claim_error) - 1) == 0 &&
        ds_host_retry_claim(remote)) return 1;
    if (remote->link == DS_LINK_SETUP &&
        frame->size == sizeof(DS_SETUP_RETRY_ERROR) - 1 &&
        memcmp(frame->data, DS_SETUP_RETRY_ERROR, frame->size) == 0) {
      int retry;
      if (remote->setup_failures < DS_SETUP_MAX_BACKOFF_EXPONENT)
        ++remote->setup_failures;
      retry = remote->listed || remote->setup_failures <= DS_SETUP_RETRY_LIMIT;
      ds_host_drop(collection, remote, retry ? "could not start sieve pool; retrying setup" :
                   "could not start sieve pool; setup retry limit reached", !retry);
      if (retry) {
        remote->retry = ds_now() + (1U << remote->setup_failures);
        remote->tried = 0;
      }
      return 1;
    }
    /* Print a bounded diagnostic. The worker has already decided to end
     * this attempt and the connection cannot accept more assignments. */
    if (frame->size != 0) {
      char diagnostic[513];
      size_t i, size = frame->size < 512 ? frame->size : 512;
      for (i = 0; i < size; ++i)
        diagnostic[i] = frame->data[i] >= 0x20 && frame->data[i] <= 0x7e ? (char)frame->data[i] : '?';
      diagnostic[size] = '\0';
      fprintf(stderr, "DMSIQS: worker %s: %.*s\n", remote->endpoint,
              (int)size, diagnostic);
    }
    ds_host_drop(collection, remote, "rejected this session", 1);
    return 1;
  }
  switch (remote->link) {
    case DS_LINK_CLAIM:
      if (frame->type == DS_BUSY && ds_reader_done(&r)) {
        if (ctx->verbose > 1 && !remote->busy_announced) {
          printf("# dmsiqs worker %s busy, waiting for a lease\n", remote->endpoint);
          fflush(stdout);
          remote->busy_announced = 1;
        }
        ds_host_close_peer(remote);
        remote->busy = 1;
        return 1;
      }
      if (frame->type != DS_GRANTED || !ds_host_identify(remote, &r, ctx->verbose)) return 0;
      remote->busy_announced = 0;
      remote->link = DS_LINK_LEASE;
      remote->deadline = 0;
      return 1;
    case DS_LINK_SETUP:
      if (frame->type == DS_READY) {
        uint32_t capacity = ds_get_u32(&r);
        if (!ds_reader_done(&r) || capacity < 2 || capacity % 2 != 0 ||
            capacity > 2U * remote->report->threads) return 0;
        remote->tickets = siqs_calloc(capacity, sizeof(*remote->tickets));
        remote->capacity = capacity;
        remote->link = DS_LINK_READY;
        remote->setup_failures = 0;
        remote->deadline = 0;
        remote->last_progress = 0;
        state->ever_ready = 1;
        if (ctx->verbose > 0) {
          const ds_worker_report *report = remote->report;
          char name[128], hostname[128], system[128], machine[128];
          printf("# dmsiqs worker %s (%s) ready: %u sieve threads, ",
                 ds_host_print_name(name, report->name), remote->endpoint, capacity / 2U);
          if (strcmp(report->name, report->hostname) != 0)
            printf("%s, ", ds_host_print_name(hostname, report->hostname));
          printf("%s %s", ds_host_print_name(system, report->system),
                 ds_host_print_name(machine, report->machine));
          if (ctx->verbose > 1) {
            char elapsed[32];
            siqs_log_duration(elapsed, sizeof(elapsed), ds_now() - remote->setup_started);
            printf(", setup %s", elapsed);
          }
          putchar('\n');
          fflush(stdout);
        }
        return 1;
      }
      return 0;
    case DS_LINK_READY:
    case DS_LINK_PAUSING:
      switch (frame->type) {
        case DS_DONE: {
          uint64_t id = ds_get_u64(&r);
          uint32_t performed = ds_get_u32(&r), i;
          if (!id || !ds_reader_done(&r)) return 0;
          for (i = 0; i < remote->capacity && remote->tickets[i].id != id; ++i) {}
          if (i == remote->capacity || performed > remote->tickets[i].limit) return 0;
          *collection->polynomials += performed;
          state->reserved -= remote->tickets[i].limit;
          remote->tickets[i].id = 0;
          --remote->outstanding;
          if (performed != 0 || remote->outstanding == 0)
            ds_host_note_progress(state, remote, performed != 0);
          remote->report->polynomials += performed;
          if (performed != 0) {
            if (ctx->journal == NULL) ++*collection->families;
            ++remote->report->families;
          }
          return 1;
        }
        case DS_RELATIONS:
        case DS_RELATIONS_CPU: {
          ds_collection_resources *resources = collection->resources;
          uint64_t accepted = ds_host_accepted(ctx);
          uint32_t records;
          int have_cpu = frame->type == DS_RELATIONS_CPU;
          int valid;
          double import_start;
          ds_cpu_usage cpu;
          if (have_cpu) {
            ds_reader report;
            if (!(remote->claim_features & DS_CLAIM_CPU_CHECKPOINTS) ||
                r.left < DS_CPU_USAGE_BYTES) return 0;
            r.left -= DS_CPU_USAGE_BYTES;
            report = (ds_reader){r.data + r.left, DS_CPU_USAGE_BYTES, 0};
            if (!ds_host_read_cpu(remote, &report, &cpu)) return 0;
          }
          /* A durable batch and its CPU snapshot should cover the same frames. */
          if (ctx->journal != NULL) ctx->journal->defer_flush = 1;
          import_start = resources != NULL ? ds_now() : 0;
          valid = ds_import_relations(ctx, &r, &records);
          if (resources != NULL) resources->import_seconds += ds_now() - import_start;
          if (ctx->journal != NULL) ctx->journal->defer_flush = 0;
          if (!valid) return 0;
          if (have_cpu) ds_host_record_batch_cpu(remote, cpu);
          if (ctx->journal != NULL) sj_maybe_flush(ctx);
          if (records != 0) ds_host_note_progress(state, remote, 1);
          remote->report->received += records;
          remote->report->accepted += ds_host_accepted(ctx) - accepted;
          remote->report->bytes += frame->size + 12U;
          if (resources != NULL) {
            resources->received += records;
            resources->wire_bytes += frame->size + 12U;
          }
          return 1;
        }
        case DS_DIVISOR:
          return ds_import_divisor(ctx, &r);
        case DS_PAUSED:
          if (remote->link != DS_LINK_PAUSING || remote->outstanding != 0 ||
              !ds_reader_done(&r)) return 0;
          remote->link = DS_LINK_PAUSED;
          remote->deadline = 0;
          return 1;
        default:
          return 0;
      }
    default:
      return 0;
  }
}

/* Cap frames per pass so a heavily loaded worker cannot starve its peers. */
static int ds_host_service(ds_collection *collection, ds_remote *remote,
                           const ds_buf *setup, int reconnect) {
  const siqs_ctx_t *ctx = collection->ctx;
  int progress = 0, frame_index;
  double now = ds_now();
  if (remote->peer == NULL)
    return reconnect ? ds_host_dial(remote, now) : 0;
  for (frame_index = 0; frame_index < 4 && remote->peer != NULL; ++frame_index) {
    ds_frame frame = {0, NULL, 0};
    int received = ds_peer_recv(remote->peer, &frame, 0);
    if (received == 0) break;
    progress = 1;
    if (received < 0) {
      if (!ds_host_retry_claim(remote))
        ds_host_drop(collection, remote, "disconnected", 0);
    } else if (!ds_host_frame(collection, remote, &frame)) {
      ds_host_drop(collection, remote, "sent an invalid frame", 1);
    }
    ds_frame_clear(&frame);
  }
  if (remote->peer == NULL) return progress;
  if (remote->deadline != 0 && now >= remote->deadline) {
    if (!ds_host_retry_claim(remote))
      ds_host_drop(collection, remote,
                   remote->link == DS_LINK_SETUP ? "timed out waiting for setup" : "timed out", 0);
    return 1;
  }
  if (remote->link == DS_LINK_READY && remote->progress_deadline != 0 &&
      now >= remote->progress_deadline) {
    ds_host_registry *registry = &collection->state->registry;
    char reason[96];
    snprintf(reason, sizeof(reason), "made no sieve progress for %u seconds",
             (unsigned)collection->state->worker_stall_seconds);
    ds_host_drop(collection, remote, reason, 0);
    remote->stall_retry = ds_now() + DS_STALL_RETRY_SECONDS;
    if (registry->endpoint != NULL) {
      remote->stall_pending = 1;
      registry->next_poll = 0;
    }
    return 1;
  }
  if (reconnect && remote->link == DS_LINK_LEASE) {
    if (!ds_peer_send(remote->peer, DS_SETUP, setup)) {
      ds_host_drop(collection, remote, "could not receive setup", 0);
    } else {
      ++remote->report->jobs;
      remote->job_cpu = (ds_cpu_usage){0, 0};
      remote->have_checkpoint = 0;
      remote->link = DS_LINK_SETUP;
      remote->setup_started = now;
      remote->deadline = now + 120.0;
      if (ctx->verbose > 1) {
        char elapsed[32], name[128];
        siqs_log_duration(elapsed, sizeof(elapsed), siqs_log_now() - ctx->collection_started);
        printf("# dmsiqs worker %s (%s) starting setup at %s (%zu bytes queued)\n",
               ds_host_print_name(name, remote->report->name), remote->endpoint,
               elapsed, setup->size);
        fflush(stdout);
      }
    }
    progress = 1;
  }
  return progress;
}

static int ds_host_assign(ds_collection *collection, ds_remote *remote,
                          siqs_poly_t *dispatch, uint32_t limit, ds_buf *family) {
  ds_host_state *state = collection->state;
  uint64_t id = ++state->next_id;
  uint32_t i, ticket;
  for (ticket = 0; ticket < remote->capacity && remote->tickets[ticket].id != 0; ++ticket) {}
  if (ticket == remote->capacity) croak("DMSIQS: assignment capacity mismatch");
  family->size = 0;
  ds_put_u64(family, id);
  ds_put_u32(family, limit);
  for (i = 0; i < dispatch->q_count; ++i) ds_put_u32(family, dispatch->a_index[i]);
  if (family->bad) croak("DMSIQS: failed to encode family");
  if (!ds_peer_send(remote->peer, DS_FAMILY, family)) {
    ds_host_drop(collection, remote, "could not receive another family", 0);
    return 0;
  }
  remote->tickets[ticket].id = id;
  remote->tickets[ticket].limit = limit;
  ++remote->outstanding;
  if (remote->outstanding == 1) ds_host_note_progress(state, remote, 0);
  state->reserved += limit;
  remote->link = DS_LINK_READY;
  return 1;
}

static void ds_host_pause(ds_collection *collection) {
  ds_host_state *state = collection->state;
  double deadline = ds_now() + 60.0;
  size_t i;
  int pending;
  for (i = 0; i < state->count; ++i) {
    ds_remote *remote = state->remotes[i];
    if (remote->link != DS_LINK_READY) continue;
    if (!ds_peer_send(remote->peer, DS_PAUSE, NULL))
      ds_host_drop(collection, remote, "disconnected before pause", 0);
    else {
      remote->link = DS_LINK_PAUSING;
      remote->deadline = deadline;
    }
  }
  do {
    int progress = 0;
    pending = 0;
    for (i = 0; i < state->count; ++i) {
      ds_remote *remote = state->remotes[i];
      if (remote->link != DS_LINK_PAUSING) continue;
      progress |= ds_host_service(collection, remote, NULL, 0);
      pending |= remote->link == DS_LINK_PAUSING;
    }
    if (pending && !progress) ds_host_idle();
  } while (pending);
}

static const char *ds_host_progress_state(const ds_remote *remote, double now) {
  if (remote->connecting) return "connecting";
  switch (remote->link) {
    case DS_LINK_CLAIM:   return "claiming";
    case DS_LINK_LEASE:   return "claimed";
    case DS_LINK_SETUP:   return "setup";
    case DS_LINK_READY:   return remote->outstanding != 0 ? "sieving" : "ready";
    case DS_LINK_PAUSING: return "draining";
    case DS_LINK_PAUSED:  return "paused";
    case DS_LINK_ENDING:  return "finishing";
    case DS_LINK_DOWN:    break;
  }
  if (remote->stall_pending || now < remote->stall_retry) return "stalled";
  if (remote->busy) return "busy";
  if (remote->rejected) return "rejected";
  return remote->tried ? "disconnected" : "waiting";
}

/* A zero interval sets the baseline before starting or resuming collection. */
static void ds_host_sample_progress(ds_host_state *state, double now, double interval,
                                     int verbose) {
  size_t i;
  for (i = 0; i < state->count; ++i) {
    ds_remote *remote = state->remotes[i];
    ds_worker_report *current = remote->report != NULL ? remote->report : remote->reports;
    ds_worker_report *report;
    double progress_age = now - remote->last_progress;
    int visible = remote->configured || remote->listed || remote->peer != NULL ||
                  remote->connecting || remote->stall_pending;
    int hide_active_row = verbose == 2 && state->count == 1 &&
                          remote->reports != NULL && remote->reports->next == NULL &&
                          remote->link == DS_LINK_READY && remote->outstanding != 0 &&
                          remote->last_progress != 0 && progress_age < 1.0;
    if (interval > 0 && current == NULL && visible)
      printf("#   %s %s\n", remote->endpoint, ds_host_progress_state(remote, now));
    for (report = remote->reports; report != NULL; report = report->next) {
      uint64_t new_polynomials = report->polynomials - report->polynomials_at_report;
      uint64_t new_originals = report->accepted - report->originals_at_report;
      char name[128];
      report->polynomials_at_report = report->polynomials;
      report->originals_at_report = report->accepted;
      if (interval == 0 || (new_polynomials == 0 && new_originals == 0 &&
                           (report != current || !visible)))
        continue;
      if (hide_active_row && (new_polynomials != 0 || new_originals != 0)) continue;
      printf("#   %s", ds_host_print_name(name, report->name));
      if (verbose > 2) printf(" (%s)", remote->endpoint);
      if (report == current && remote->link == DS_LINK_SETUP &&
          new_polynomials == 0 && new_originals == 0) {
        char elapsed[32];
        siqs_log_duration(elapsed, sizeof(elapsed), now - remote->setup_started);
        printf(" setting up for %s\n", elapsed);
        continue;
      }
      printf(" %s: %.1f polys/s, %.1f rels/s",
             report == current ? ds_host_progress_state(remote, now) : "disconnected",
             new_polynomials / interval, new_originals / interval);
      if (report == current && remote->last_progress == 0)
        printf(", no progress yet");
      else if (report == current && progress_age >= 1.0)
        printf(", last progress %.0f s ago", progress_age);
      putchar('\n');
    }
  }
}

static void ds_host_checkpoint_report(siqs_journal_t *journal, unsigned interval, int verbose) {
  uint64_t age_us;
  if (journal == NULL) return;
  if (!sj_checkpoint_age(journal, &age_us))
    printf("#   no journal checkpoint yet\n");
  else if (age_us >= (uint64_t)interval * UINT64_C(1000000) || verbose > 2) {
    if (age_us < UINT64_C(1000000))
      printf("#   journal checkpoint <1 s ago\n");
    else
      printf("#   journal checkpoint %llu s ago\n", (unsigned long long)(age_us / UINT64_C(1000000)));
  }
}

static int ds_collect_relations(siqs_ctx_t *ctx, siqs_poly_t *dispatch,
                                uint32_t target, uint32_t *next_matrix_check,
                                uint64_t *family_count, uint64_t *poly_count) {
  ds_host_state *state = ds_host_get(ctx->distributed);
  ds_collection_resources resources = {0};
  ds_collection collection = {ctx, state, family_count, poly_count, NULL};
  ds_collection_timing timing = {0};
  ds_buf setup = {0}, family = {0};
  uint64_t max_polynomials = siqs_polynomial_limit(&ctx->params);
  uint64_t journal_start = 0;
  uint32_t check_interval = ctx->params.fb_size / 128;
  unsigned report_interval = state->worker_report_seconds != 0
                               ? state->worker_report_seconds : DMSIQS_DEFAULT_WORKER_REPORT_SECONDS;
  double loss_deadline = 0, worker_report_at = 0;
  int time_phases = ctx->verbose > 0 || ctx->journal != NULL;
  int show_worker_progress = ctx->verbose > 1 && state->worker_report_seconds != 0;
  int exhausted = 0, complete = 0, connect_failed = 0, waiting = 0;
  size_t i;

  if (ctx->factor_found || siqs_relation_target_reached(ctx, target)) return 1;
  if (*poly_count >= max_polynomials) return 0;
  if (time_phases) timing.since = ds_now();
  if (ctx->verbose > 1 && !state->targets_announced) {
    const ds_host *host = ctx->distributed;
    if (host->count != 0) {
      printf("# dmsiqs configured workers:");
      for (i = 0; i < host->count; ++i) printf("%s%s", i == 0 ? " " : ", ", host->endpoints[i]);
      putchar('\n');
    }
    if (state->registry.endpoint != NULL)
      printf("# dmsiqs discovering workers at %s, pool %s\n",
             state->registry.endpoint, state->registry.pool);
    fflush(stdout);
    state->targets_announced = 1;
  }
  if (ctx->verbose > 2) {
    collection.resources = &resources;
    if (ctx->journal != NULL)
      journal_start = ctx->journal->need_position ? ctx->journal->append_offset : sj_tell(ctx->journal);
  }
  if (ctx->journal != NULL && state->next_id < *family_count)
    state->next_id = *family_count;
  if (!ds_encode_setup(&setup, ctx)) croak("DMSIQS: failed to encode setup");
  if (check_interval < SIQS_MATRIX_CHECK_MIN) check_interval = SIQS_MATRIX_CHECK_MIN;
  if (check_interval > SIQS_MATRIX_CHECK_MAX) check_interval = SIQS_MATRIX_CHECK_MAX;
  if (show_worker_progress) {
    worker_report_at = ds_now();
    ds_host_sample_progress(state, worker_report_at, 0, ctx->verbose);
  }
  for (;;) {
    uint64_t remaining;
    uint32_t sieve_threads = 0;
    int progress = ds_host_poll_registry(state, ctx->verbose);
    int pending = 0, busy = 0, eligible = 0;
    size_t usable = 0;
    for (i = 0; i < state->count; ++i) {
      ds_remote *remote = state->remotes[(state->first + i) % state->count];
      progress |= ds_host_service(&collection, remote, &setup, !exhausted);
      busy |= remote->busy;
      eligible |= !remote->rejected;
      pending |= remote->connecting || !remote->tried || remote->link == DS_LINK_CLAIM ||
                 remote->link == DS_LINK_LEASE || remote->link == DS_LINK_SETUP;
      if (remote->link == DS_LINK_READY || remote->link == DS_LINK_PAUSED) {
        ++usable;
        if (sieve_threads <= UINT32_MAX - remote->capacity / 2U)
          sieve_threads += remote->capacity / 2U;
      }
    }
    if (state->count) state->first = (state->first + 1U) % state->count;
    if (time_phases) {
      double now = ds_now();
      if (usable || timing.phase != DS_PHASE_STARTUP)
        ds_host_change_phase(&timing, usable ? DS_PHASE_COLLECTION : DS_PHASE_WAITING, now);
#ifndef _WIN32
      if (ctx->journal != NULL)
        sj_check_space(ctx->journal, sj_clock_us(ctx->journal, 1),
                       SJ_HEADER + (uint64_t)ctx->journal->batch.size);
#endif
    }
    if (ctx->factor_found || siqs_relation_target_reached(ctx, target)) {
      complete = 1;
      break;
    }
    if (siqs_relation_count(ctx) >= *next_matrix_check) {
      uint32_t rows, columns;
      complete = siqs_matrix_ready(ctx, &rows, &columns);
      *next_matrix_check = siqs_relation_count(ctx) + check_interval;
      siqs_print_matrix_readiness(ctx, rows, columns, complete);
      if (complete) break;
    }
    if (state->reserved == 0 && (exhausted || *poly_count >= max_polynomials)) break;
    if (siqs_progress_due(ctx, *poly_count, 0, report_interval)) {
      double now = ds_now();
      siqs_print_polynomial_notice(ctx, *poly_count);
      siqs_print_relation_report(ctx, target, *poly_count);
      printf("#   %zu worker%s, %u sieve thread%s\n", usable, usable == 1 ? "" : "s",
             sieve_threads, sieve_threads == 1U ? "" : "s");
      ds_host_checkpoint_report(ctx->journal, report_interval, ctx->verbose);
      if (show_worker_progress && now > worker_report_at) {
        ds_host_sample_progress(state, now, now - worker_report_at, ctx->verbose);
        worker_report_at = now;
      }
      fflush(stdout);
    }
    if (usable) {
      loss_deadline = 0;
      waiting = 0;
    } else if (state->registry.endpoint != NULL) {
      if (!waiting && ctx->verbose > 0) {
        printf("# dmsiqs waiting for workers in coordinator pool %s\n", state->registry.pool);
        fflush(stdout);
      }
      waiting = 1;
    } else if (!eligible || (!state->ever_ready && !pending && !busy)) {
      fprintf(stderr, "DMSIQS: no compatible workers available\n");
      connect_failed = 1;
      break;
    } else if (state->ever_ready) {
      if (loss_deadline == 0) loss_deadline = ds_now() + 60.0;
      if (ds_now() >= loss_deadline) {
        fprintf(stderr, "DMSIQS: no worker reconnected within 60 seconds\n");
        connect_failed = 1;
        break;
      }
    }

    for (i = 0; i < state->count && !exhausted; ++i) {
      ds_remote *remote = state->remotes[(state->first + i) % state->count];
      if (remote->link != DS_LINK_READY && remote->link != DS_LINK_PAUSED) continue;
      while (remote->outstanding < remote->capacity) {
        uint32_t limit;
        remaining = max_polynomials - *poly_count - state->reserved;
        if (remaining == 0) break;
        if (!ds_choose_family(ctx, dispatch)) {
          exhausted = 1;
          break;
        }
        /* Record every chooser advance, even if sending this family fails. */
        if (ctx->journal != NULL) {
          siqs_journal_family(ctx, dispatch);
          ++*family_count;
        }
        limit = remaining < dispatch->b_limit ? (uint32_t)remaining : dispatch->b_limit;
        progress = 1;
        if (!ds_host_assign(&collection, remote, dispatch, limit, &family)) break;
      }
    }
    if (!progress) {
      if (ctx->journal != NULL) sj_maybe_flush(ctx);
      ds_host_idle();
    }
  }
  if (time_phases) ds_host_change_phase(&timing, DS_PHASE_DRAIN, ds_now());
  ds_host_pause(&collection);
  if (time_phases) {
    ds_host_change_phase(&timing, DS_PHASE_COUNT, ds_now());
    sj_write_collection(ctx, timing.us[DS_PHASE_STARTUP], timing.us[DS_PHASE_COLLECTION],
                        timing.us[DS_PHASE_WAITING], timing.us[DS_PHASE_DRAIN]);
    if (ctx->verbose > 0) {
      char startup[40], active[40], waiting_time[40], drain[40];
      siqs_log_duration(startup, sizeof(startup), timing.us[DS_PHASE_STARTUP] / 1e6);
      siqs_log_duration(active, sizeof(active), timing.us[DS_PHASE_COLLECTION] / 1e6);
      siqs_log_duration(drain, sizeof(drain), timing.us[DS_PHASE_DRAIN] / 1e6);
      printf("# dmsiqs phases: startup %s, active %s", startup, active);
      if (timing.us[DS_PHASE_WAITING] != 0) {
        siqs_log_duration(waiting_time, sizeof(waiting_time), timing.us[DS_PHASE_WAITING] / 1e6);
        printf(", waiting %s", waiting_time);
      }
      printf(", drain %s\n", drain);
    }
    if (ctx->verbose > 2) {
      printf("# dmsiqs collection resources: %llu originals received, %llu relation wire bytes, %.3f s importing relations",
             (unsigned long long)resources.received, (unsigned long long)resources.wire_bytes,
             resources.import_seconds);
      if (ctx->journal != NULL) {
        uint64_t journal_end = sj_tell(ctx->journal);
        printf(", +%llu journal bytes (%llu total)", (unsigned long long)(journal_end - journal_start),
               (unsigned long long)journal_end);
      }
      putchar('\n');
    }
    if (ctx->verbose > 0) fflush(stdout);
  }
  ds_buf_clear(&setup);
  ds_buf_clear(&family);
  if (ctx->verbose > 0) siqs_print_polynomial_notice(ctx, *poly_count);
  if (complete || ctx->factor_found || siqs_relation_target_reached(ctx, target)) return 1;
  if (siqs_relation_count(ctx) > ctx->matrix_last_count) {
    uint32_t rows, columns;
    if (siqs_matrix_ready(ctx, &rows, &columns)) return 1;
  }
  if (connect_failed) ctx->distributed->failed = 1;
  return 0;
}

static int ds_host_record_ended(ds_remote *remote, const ds_frame *frame) {
  ds_reader reader = {frame->data, frame->size, 0};
  ds_worker_report *report = remote->report;
  ds_cpu_usage cpu;
  uint64_t peak_rss_bytes = 0;
  if (frame->type != DS_ENDED) return 0;
  if (frame->size == 0) return 1;
  if (remote->claim_features & DS_CLAIM_PEAK_RSS) {
    ds_reader rss;
    if (frame->size != DS_ENDED_USAGE_BYTES) return 0;
    reader.left = DS_CPU_USAGE_BYTES;
    rss = (ds_reader){frame->data + reader.left, frame->size - reader.left, 0};
    peak_rss_bytes = ds_get_u64(&rss);
  }
  if (report->timed_jobs >= report->jobs || !ds_host_read_cpu(remote, &reader, &cpu)) return 0;
  ds_host_add_cpu(remote, cpu);
  if (peak_rss_bytes > report->peak_rss_bytes) report->peak_rss_bytes = peak_rss_bytes;
  if (remote->have_checkpoint) {
    --report->checkpointed_jobs;
    remote->have_checkpoint = 0;
  }
  ++report->timed_jobs;
  return 1;
}

static void ds_host_end_attempt(ds_host *host) {
  ds_host_state *state = host->state;
  size_t i;
  int pending;
  double deadline = ds_now() + 10.0;
  if (state == NULL) return;
  for (i = 0; i < state->count; ++i) {
    ds_remote *remote = state->remotes[i];
    remote->rejected = 0;
    remote->setup_failures = 0;
    if (remote->link == DS_LINK_PAUSED || remote->link == DS_LINK_READY) {
      if (ds_peer_send(remote->peer, DS_END, NULL)) remote->link = DS_LINK_ENDING;
      else ds_host_close_peer(remote);
    } else if (remote->link == DS_LINK_SETUP) {
      ds_host_close_peer(remote);
    }
  }
  do {
    pending = 0;
    for (i = 0; i < state->count; ++i) {
      ds_remote *remote = state->remotes[i];
      ds_frame frame = {0, NULL, 0};
      int received;
      if (remote->link != DS_LINK_ENDING) continue;
      received = ds_peer_recv(remote->peer, &frame, 0);
      if (received == 1 && ds_host_record_ended(remote, &frame)) {
        free(remote->tickets);
        remote->tickets = NULL;
        remote->capacity = 0;
        remote->link = DS_LINK_LEASE;
      } else if (received != 0 || ds_now() >= deadline) ds_host_close_peer(remote);
      else pending = 1;
      ds_frame_clear(&frame);
    }
    if (pending) ds_host_idle();
  } while (pending);
}

static double ds_host_core_hours(ds_cpu_usage cpu) {
  return ((double)cpu.user_us + (double)cpu.system_us) / 3600000000.0;
}

static void ds_host_account_worker(sj_accounting *usage, const ds_worker_report *report) {
  usage->worker_user_us += report->cpu.user_us;
  usage->worker_system_us += report->cpu.system_us;
  usage->jobs += report->jobs;
  usage->timed_jobs += report->timed_jobs;
  usage->checkpointed_jobs += report->checkpointed_jobs;
}

static void ds_host_measure_process(sj_accounting *usage) {
  ds_cpu_usage cpu;
  if (ds_process_usage(&cpu, &usage->peak_rss_bytes)) {
    usage->flags |= SJ_ACCOUNTING_HOST_CPU;
    usage->host_user_us = cpu.user_us;
    usage->host_system_us = cpu.system_us;
  }
}

static void ds_host_checkpoint(siqs_ctx_t *ctx) {
  ds_host_state *state = ctx->distributed->state;
  sj_accounting usage = {0};
  size_t i;
  if (state != NULL) {
    for (i = 0; i < state->count; ++i) {
      const ds_worker_report *report;
      for (report = state->remotes[i]->reports; report != NULL; report = report->next)
        ds_host_account_worker(&usage, report);
    }
  }
  ds_host_measure_process(&usage);
  sj_write_accounting(ctx->journal, &usage);
}

static void ds_host_print_cpu_coverage(uint64_t jobs, uint64_t timed_jobs,
                                      uint64_t checkpointed_jobs) {
  printf(" (%llu/%llu jobs reported", (unsigned long long)timed_jobs,
         (unsigned long long)jobs);
  if (checkpointed_jobs != 0)
    printf(", %llu checkpoint-only", (unsigned long long)checkpointed_jobs);
  putchar(')');
}

static void ds_host_print_cpu(const char *label, const sj_accounting *usage, int partial, int verbose) {
  int precision = verbose > 2 ? 6 : 4;
  double worker_hours = ds_host_core_hours((ds_cpu_usage){usage->worker_user_us, usage->worker_system_us});
  printf("# dmsiqs %s: %.*f worker core-hours", label, precision, worker_hours);
  ds_host_print_cpu_coverage(usage->jobs, usage->timed_jobs, usage->checkpointed_jobs);
  if (usage->flags & SJ_ACCOUNTING_HOST_CPU) {
    double host_hours = ds_host_core_hours((ds_cpu_usage){usage->host_user_us, usage->host_system_us});
    printf(", %.*f host core-hours, %.*f %stotal core-hours",
           precision, host_hours, precision, worker_hours + host_hours, partial ? "partial " : "");
  } else {
    printf(", host CPU unavailable, %.*f partial total core-hours", precision, worker_hours);
  }
}

static void ds_host_finish_accounting(siqs_journal_t *journal, sj_accounting *usage, int verbose) {
  sj_accounting_totals totals;
  int current_run = journal == NULL || journal->writing_run || !sj_peek(journal);
  if (current_run) {
    ds_host_measure_process(usage);
    if (verbose > 0 && usage->jobs != 0) {
      ds_host_print_cpu("CPU", usage, usage->timed_jobs != usage->jobs, verbose);
      putchar('\n');
    }
    if (journal != NULL) {
      usage->flags |= SJ_ACCOUNTING_FINAL;
      sj_write_accounting(journal, usage);
    }
  }
  if (verbose == 0) return;
  if (current_run && usage->peak_rss_bytes != 0)
    printf("# dmsiqs host peak RSS: %.1f MiB\n",
           (double)usage->peak_rss_bytes / (1024.0 * 1024.0));
  if (journal != NULL) {
    sj_get_accounting(journal, &totals);
    if (totals.runs != 0 && (!current_run || totals.journal_runs > 1 || usage->jobs == 0)) {
      int partial = totals.final_runs != totals.journal_runs ||
                    totals.host_cpu_runs != totals.journal_runs ||
                    totals.usage.timed_jobs != totals.usage.jobs;
      ds_host_print_cpu("journal CPU", &totals.usage, partial, verbose);
      printf(" (%llu/%llu invocations recorded, %llu final)\n",
             (unsigned long long)totals.runs, (unsigned long long)totals.journal_runs,
             (unsigned long long)totals.final_runs);
    }
    if (totals.usage.peak_rss_bytes != 0 &&
        (!current_run || totals.usage.peak_rss_bytes != usage->peak_rss_bytes))
      printf("# dmsiqs journal host peak RSS: %.1f MiB\n",
             (double)totals.usage.peak_rss_bytes / (1024.0 * 1024.0));
  }
}

static void ds_host_print_worker(const ds_worker_report *report, const char *endpoint,
                                  uint64_t confirmed, int verbose) {
  char name[128];
  double share = confirmed ? 100.0 * (double)report->polynomials / (double)confirmed : 0.0;
  printf("# dmsiqs worker %s", ds_host_print_name(name, report->name));
  if (verbose > 2) {
    char hostname[128], system[128], machine[128];
    printf(" (%s, %s %s, %u threads, %s): %llu families with confirmed work, "
           "%llu confirmed polynomials (%.1f%%), %llu/%llu originals accepted/received, %llu relation wire bytes",
           ds_host_print_name(hostname, report->hostname), ds_host_print_name(system, report->system),
           ds_host_print_name(machine, report->machine), report->threads, endpoint,
           (unsigned long long)report->families, (unsigned long long)report->polynomials, share,
           (unsigned long long)report->accepted, (unsigned long long)report->received,
           (unsigned long long)report->bytes);
    if (report->input_peak_bytes != 0)
      printf(", peak receive queue %zu bytes", report->input_peak_bytes);
  } else {
    printf(": %.1f%% of confirmed polys, %llu", share, (unsigned long long)report->accepted);
    if (report->received != report->accepted)
      printf("/%llu originals accepted/received", (unsigned long long)report->received);
    else
      printf(" originals accepted");
  }
  if (report->jobs != 0) {
    double hours = ds_host_core_hours(report->cpu);
    if (report->timed_jobs != 0 || report->checkpointed_jobs != 0)
      printf(", %.*f core-hours", verbose > 2 ? 6 : 4, hours);
    else printf(", CPU unavailable");
    if (verbose > 2 || report->timed_jobs != report->jobs)
      ds_host_print_cpu_coverage(report->jobs, report->timed_jobs, report->checkpointed_jobs);
  }
  if (report->peak_rss_bytes != 0)
    printf(", peak RSS %.1f MiB", (double)report->peak_rss_bytes / (1024.0 * 1024.0));
  putchar('\n');
}

static void ds_host_close(ds_host *host, siqs_journal_t *journal, int verbose) {
  ds_host_state *state = host->state;
  sj_accounting usage = {0};
  uint64_t confirmed = 0;
  size_t i;
  if (state == NULL) {
    if (journal != NULL) ds_host_finish_accounting(journal, &usage, verbose);
    return;
  }
  if (state->registry.endpoint != NULL) {
    if (state->registry.running) {
      pthread_join(state->registry.thread, NULL);
      if (state->registry.stall_answered) state->registry.stall_remote->stall_pending = 0;
    }
    for (i = 0; i < state->count; ++i) {
      ds_remote *remote = state->remotes[i];
      if (remote->stall_pending &&
          !ds_registry_report_stall(state->registry.endpoint, state->registry.pool,
                                    remote->endpoint))
        break;
    }
    ds_registry_free(state->registry.entries, state->registry.count);
    pthread_mutex_destroy(&state->registry.mutex);
  }
  for (i = 0; i < state->count; ++i) {
    ds_worker_report *report;
    for (report = state->remotes[i]->reports; report != NULL; report = report->next)
      confirmed += report->polynomials;
  }
  for (i = 0; i < state->count; ++i) {
    ds_remote *remote = state->remotes[i];
    if (remote->connecting) {
      pthread_join(remote->connector, NULL);
      if (remote->connected_fd >= 0) close(remote->connected_fd);
    }
    ds_host_close_peer(remote);
    pthread_mutex_destroy(&remote->connect_mutex);
    while (remote->reports != NULL) {
      ds_worker_report *report = remote->reports;
      remote->reports = report->next;
      if (verbose > 0) ds_host_print_worker(report, remote->endpoint, confirmed, verbose);
      if (report->jobs != 0) ds_host_account_worker(&usage, report);
      free(report);
    }
    free(remote->endpoint);
    free(remote);
  }
  if (verbose > 0 && (state->uncertain || (verbose > 2 && confirmed)))
    printf("# dmsiqs work: %llu confirmed polys, %llu unconfirmed polys charged after disconnect\n",
           (unsigned long long)confirmed, (unsigned long long)state->uncertain);
  free(state->remotes);
  free(state);
  host->state = NULL;
  ds_host_finish_accounting(journal, &usage, verbose);
}
