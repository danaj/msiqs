/* Compile/link probe for the unsigned counters used by residual-first SIQS. */
static volatile unsigned jobs;

int main(void) {
  unsigned active = __atomic_add_fetch(&jobs, 1U, __ATOMIC_RELAXED);
  unsigned expected = __atomic_load_n(&jobs, __ATOMIC_RELAXED);
  (void)__atomic_compare_exchange_n(&jobs, &expected, active, 0,
                                     __ATOMIC_RELAXED, __ATOMIC_RELAXED);
  return __atomic_sub_fetch(&jobs, 1U, __ATOMIC_RELAXED) != 0;
}
