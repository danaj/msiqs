/* Advisory policy for a journal's committed writes. The caller supplies the
 * monotonic clock and available space, so this file has no filesystem reads. */
#include <stdint.h>

#define SJ_SPACE_MINUTE_US UINT64_C(60000000)
#define SJ_SPACE_WINDOW_MINUTES 5U
#define SJ_SPACE_WINDOW_US (SJ_SPACE_WINDOW_MINUTES * SJ_SPACE_MINUTE_US)
#define SJ_SPACE_REPEAT_US (5U * SJ_SPACE_MINUTE_US)
#define SJ_SPACE_WARN_MINUTES 30.0

enum sj_space_warning {
  SJ_SPACE_NO_WARNING, SJ_SPACE_WARN_RATE, SJ_SPACE_WARN_CHECKPOINT
};

typedef struct {
  uint64_t time_us, committed_bytes;
} sj_space_sample;

typedef struct {
  sj_space_sample samples[SJ_SPACE_WINDOW_MINUTES + 1U];
  unsigned count;
  uint64_t last_warning_us;
  int warning_active;
} sj_space_state;

static int sj_space_due(const sj_space_state *state, uint64_t now_us) {
  uint64_t last_us;
  if (state->count == 0) return 1;
  last_us = state->samples[state->count - 1U].time_us;
  return now_us < last_us || now_us - last_us >= SJ_SPACE_MINUTE_US;
}

static enum sj_space_warning sj_space_update(sj_space_state *state, uint64_t now_us,
    uint64_t committed_bytes, uint64_t available_bytes, uint64_t checkpoint_bytes,
    double *minutes_left, double *bytes_per_minute) {
  enum sj_space_warning warning = SJ_SPACE_NO_WARNING;
  unsigned i, retained = 0;
  *minutes_left = 0.0;
  *bytes_per_minute = 0.0;
  if (!sj_space_due(state, now_us)) return SJ_SPACE_NO_WARNING;
  if (state->count != 0) {
    sj_space_sample last = state->samples[state->count - 1U];
    if (now_us < last.time_us || committed_bytes < last.committed_bytes)
      *state = (sj_space_state){0};
  }
  for (i = 0; i < state->count; ++i) {
    sj_space_sample sample = state->samples[i];
    if (now_us - sample.time_us <= SJ_SPACE_WINDOW_US)
      state->samples[retained++] = sample;
  }
  /* Minute spacing leaves at most five older samples in this window. */
  state->samples[retained++] = (sj_space_sample){now_us, committed_bytes};
  state->count = retained;

  if (retained > 1 && committed_bytes > state->samples[0].committed_bytes) {
    uint64_t growth = committed_bytes - state->samples[0].committed_bytes;
    uint64_t elapsed = now_us - state->samples[0].time_us;
    double growth_per_minute = (double)growth * (double)SJ_SPACE_MINUTE_US;
    *minutes_left = ((double)available_bytes * (double)elapsed) / growth_per_minute;
    *bytes_per_minute = growth_per_minute / (double)elapsed;
    if (*minutes_left < SJ_SPACE_WARN_MINUTES) warning = SJ_SPACE_WARN_RATE;
  }
  if (warning == SJ_SPACE_NO_WARNING && available_bytes < checkpoint_bytes)
    warning = SJ_SPACE_WARN_CHECKPOINT;
  if (warning == SJ_SPACE_NO_WARNING) state->warning_active = 0;
  else {
    if (state->warning_active && now_us - state->last_warning_us < SJ_SPACE_REPEAT_US)
      return SJ_SPACE_NO_WARNING;
    state->warning_active = 1;
    state->last_warning_us = now_us;
  }
  return warning;
}
