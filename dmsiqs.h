#ifndef DMSIQS_H
#define DMSIQS_H

#include <stddef.h>
#include <stdint.h>
#include <gmp.h>

#include "siqs.h"

#define DMSIQS_MIN_BITS 270U
#define DMSIQS_DEFAULT_WORKER_STALL_SECONDS 600U
#define DMSIQS_DEFAULT_WORKER_REPORT_SECONDS 60U

typedef struct {
  const char *const *endpoints;
  size_t count;
  void *state;
  int failed;
  const char *coordinator, *pool;
  uint32_t worker_stall_seconds;
  uint32_t worker_report_seconds;
} ds_host;

mpz_t *dmsiqs_factor(ds_host *, const mpz_t, uint32_t *, uint32_t,
                     int, uint32_t, uint32_t, siqs_journal_t *);
void dmsiqs_close(ds_host *, siqs_journal_t *, int);
int dmsiqs_worker(const char *, const char *, uint32_t, const char *,
                  const char *, const char *, const char *, int);

#endif
