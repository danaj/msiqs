/* Deterministic checks for journal space advisories. */
#ifdef NDEBUG
# undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include "../../siqs_journal_space_inc.c"

static void check_rate_and_cooldown(void) {
  const uint64_t minute = SJ_SPACE_MINUTE_US;
  sj_space_state state = {0};
  double estimate, rate;

  assert(sj_space_due(&state, 0));
  assert(sj_space_update(&state, 0, 0, 3000, 100, &estimate, &rate) == SJ_SPACE_NO_WARNING);
  assert(!sj_space_due(&state, minute - 1));
  assert(sj_space_update(&state, minute - 1, 100, 0, 100, &estimate, &rate) == SJ_SPACE_NO_WARNING);
  assert(sj_space_due(&state, minute));
  assert(sj_space_update(&state, minute, 100, 3000, 100, &estimate, &rate) == SJ_SPACE_NO_WARNING);
  assert(estimate == 30.0);

  assert(sj_space_update(&state, 2*minute, 200, 2900, 100, &estimate, &rate) == SJ_SPACE_WARN_RATE);
  assert(estimate == 29.0);
  assert(sj_space_update(&state, 3*minute, 300, 2900, 100, &estimate, &rate) == SJ_SPACE_NO_WARNING);
  assert(sj_space_update(&state, 4*minute, 400, 2900, 100, &estimate, &rate) == SJ_SPACE_NO_WARNING);
  assert(sj_space_update(&state, 5*minute, 500, 2900, 100, &estimate, &rate) == SJ_SPACE_NO_WARNING);
  assert(sj_space_update(&state, 6*minute, 600, 2900, 100, &estimate, &rate) == SJ_SPACE_NO_WARNING);
  assert(sj_space_update(&state, 7*minute, 700, 2900, 100, &estimate, &rate) == SJ_SPACE_WARN_RATE);
  assert(sj_space_update(&state, 8*minute, 800, 3000, 100, &estimate, &rate) == SJ_SPACE_NO_WARNING);
  assert(sj_space_update(&state, 9*minute, 900, 0, 100, &estimate, &rate) == SJ_SPACE_WARN_RATE);
  assert(estimate == 0.0 && rate == 100.0);
}

static void check_checkpoint_floor(void) {
  const uint64_t minute = SJ_SPACE_MINUTE_US;
  sj_space_state state = {0};
  double estimate, rate;

  assert(sj_space_update(&state, 0, 101, 1023, 1024, &estimate, &rate) == SJ_SPACE_WARN_CHECKPOINT);
  assert(estimate == 0.0 && rate == 0.0);
  assert(sj_space_update(&state, minute, 101, 1023, 1024, &estimate, &rate) == SJ_SPACE_NO_WARNING);
  assert(sj_space_update(&state, 2*minute, 101, 1024, 1024, &estimate, &rate) == SJ_SPACE_NO_WARNING);
  assert(sj_space_update(&state, 3*minute, 101, 1023, 1024, &estimate, &rate) == SJ_SPACE_WARN_CHECKPOINT);
  assert(sj_space_update(&state, 4*minute, 101, 1024, 1024, &estimate, &rate) == SJ_SPACE_NO_WARNING);
  assert(sj_space_update(&state, 5*minute, 102, 1023, 1024, &estimate, &rate) == SJ_SPACE_WARN_CHECKPOINT);
}

static void check_elapsed_window(void) {
  const uint64_t minute = SJ_SPACE_MINUTE_US;
  sj_space_state state = {0};
  double estimate, rate;

  assert(sj_space_update(&state, 0, 0, 1000000, 1000, &estimate, &rate) == SJ_SPACE_NO_WARNING);
  assert(sj_space_update(&state, minute, 10000, 1000000, 1000, &estimate, &rate) == SJ_SPACE_NO_WARNING);
  assert(sj_space_update(&state, 6*minute, 10000, 2000, 1000, &estimate, &rate) == SJ_SPACE_NO_WARNING);
  assert(state.count == 2 && state.samples[0].time_us == minute);
  assert(sj_space_update(&state, 12*minute, 20000, 2000, 1000, &estimate, &rate) == SJ_SPACE_NO_WARNING);
  assert(state.count == 1);
  assert(sj_space_update(&state, 13*minute + minute/2, 20120, 1600, 1000, &estimate, &rate) == SJ_SPACE_WARN_RATE);
  assert(estimate == 20.0 && rate == 80.0);
}

int main(void) {
  check_rate_and_cooldown();
  check_checkpoint_floor();
  check_elapsed_window();
  puts("PASS journal space: monotonic rate, rolling window, checkpoint floor, and warning cooldown");
  return 0;
}
