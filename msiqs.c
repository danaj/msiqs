/*============================================================================

  Command-line driver for msiqs.

  Direct build from the repository root:

    cc -O3 -march=native -falign-loops=32  -DPSIQS -pthread  -o msiqs  msiqs.c siqs.c lanczos.c pr32_iter.c tinyecm64.c  -lgmp -lm

  Remove '-march=native' if building a portable binary.
  Remove '-DPSIQS -pthread' for a serial build.

  Loop alignment avoids placement-sensitive sieve slowdowns on x86.

  The local Makefile needs no module configuration: make detects pthreads,
  make serial forces a serial-only build.  Both targets produce msiqs.

  See embedded/README.md for C integration.
  Do not link this main into another driver.

  Copyright (c) 2026 Dana Jacobsen

============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <gmp.h>

#include "ptypes.h"
#include "pr32_iter.h"
#include "siqs.h"
#ifdef DMSIQS
# include "dmsiqs.h"
# include "dmsiqs_registry.h"
# define SIQS_PROGRAM_NAME "dmsiqs"
static ds_host distributed_host;
#else
#define SIQS_PROGRAM_NAME "msiqs"
#endif

static int verbose_level = 0;  /* -v summaries, -vv progress, -vvv diagnostics. */

static void print_usage(FILE *stream, const char *program) {
#ifdef DMSIQS
  fprintf(stream,
          "usage: %s [--worker ADDRESS[:PORT] ...] [--coord ADDRESS[:PORT] [--pool NAME]]\n"
          "           [-t N] [-v] [--2lp|--3lp] [--journal FILE]\n"
          "           [--worker-stall SECONDS] [--worker-report SECONDS] [INTEGER]\n"
          "       %s --mode worker [--bind ADDRESS[:PORT]] [--name NAME] [-t N]\n"
          "           [--coord ADDRESS[:PORT] [--external ADDRESS[:PORT]] [--pool NAME]]\n"
          "Host mode requires --worker or --coord. Worker addresses default to port " DS_WORKER_PORT ".\n"
          "Coordinator addresses default to port " DS_COORDINATOR_PORT ".\n"
          "Host -t sets solver threads. Worker -t sets sieve threads. Both default to 1.\n"
          "--worker-stall drops a worker with outstanding work after SECONDS without\n"
          "progress (default %u). Use 0 to disable.\n"
          "-v shows summaries, -vv adds timed progress, -vvv adds diagnostics.\n"
          "--worker-report sets the -vv progress interval in seconds (default %u).\n"
          "Use 0 to hide worker rows and keep aggregate progress.\n"
          "Workers bind to 127.0.0.1:8012 by default. --external defaults to the bind endpoint.\n"
          "A wildcard bind needs --external when using a coordinator.\n"
          "The --coordinator, --advertise, and --port spellings are also accepted.\n"
          "--version prints the worker protocol version and available Git information.\n"
          "Without INTEGER, --journal FILE resumes using the journal input.\n"
          "Use --journal FILE - to read exactly one input from stdin.\n",
          program, program, DMSIQS_DEFAULT_WORKER_STALL_SECONDS,
          DMSIQS_DEFAULT_WORKER_REPORT_SECONDS);
#else
  fprintf(stream,
#ifdef PSIQS
          "usage: %s [-v|--verbose] [-threads N] [--2lp|--3lp] [--] [INTEGER ...]\n"
          "Factor positive decimal integers with msiqs.\n"
          "Use -threads N to select 1-%u workers (default 1); "
          "one worker uses serial SIQS.\n"
          "-t and --threads are aliases for -threads.\n"
#else
          "usage: %s [-v|--verbose] [--2lp|--3lp] [--] [INTEGER ...]\n"
          "Factor positive decimal integers with msiqs.\n"
#endif
          "-v shows summaries, -vv adds timed progress, -vvv adds diagnostics.\n"
          "Default: ordinary policies through 360 bits, 3LP from 361 bits (post-trial).\n"
          "--2lp forces ordinary policies; --3lp requests 3LP wherever supported.\n"
          "These options are mutually exclusive; unsupported 3LP sizes use ordinary policies.\n"
          "Without --journal or INTEGER arguments, read integers from stdin.\n"
          "--journal FILE N creates or resumes a journal for N.\n"
          "--journal FILE resumes using N from the existing journal.\n"
          "--journal FILE - reads exactly one N from stdin.\n",
#ifdef PSIQS
          program, (unsigned)PSIQS_MAX_THREADS);
#else
          program);
#endif
#endif
}

#ifdef PSIQS
/* Reject signs, trailing text and out-of-range counts without narrowing. */
static int parse_uint32(const char *text, uint32_t *out) {
  uint64_t value = 0;
  const char *p;
  if (*text == '\0')
    return 0;
  for (p = text; *p != '\0'; p++) {
    if (*p < '0' || *p > '9')
      return 0;
    value = 10U * value + (uint32_t)(*p - '0');
    if (value > UINT32_MAX)
      return 0;
  }
  *out = (uint32_t)value;
  return 1;
}

static int parse_thread_count(const char *text, uint32_t *nthreads) {
  uint32_t count;
  if (!parse_uint32(text, &count) || count == 0 || count > PSIQS_MAX_THREADS)
    return 0;
  *nthreads = count;
  return 1;
}
#endif

typedef struct {
  mpz_t *values;
  size_t count;
  size_t allocated;
} siqs_factor_list_t;

static void append_factor(siqs_factor_list_t *list, const mpz_t factor) {
  if (list->count == list->allocated) {
    size_t next = list->allocated ? list->allocated * 2 : 16;
    mpz_t *values;
    if (next < list->allocated ||
        next > ((size_t)-1) / sizeof(*values)) {
      fprintf(stderr, SIQS_PROGRAM_NAME ": too many factors\n");
      exit(3);
    }
    values = (mpz_t *)realloc(list->values, next * sizeof(*values));
    if (values == NULL) {
      fprintf(stderr, SIQS_PROGRAM_NAME ": unable to allocate factors\n");
      exit(3);
    }
    list->values = values;
    list->allocated = next;
  }
  mpz_init_set(list->values[list->count++], factor);
}

/* Revisit only strictly smaller composites, never an unsplit cofactor. */
static int collect_factors(const mpz_t n, siqs_factor_list_t *output,
                           uint32_t nthreads, siqs_journal_t *journal,
                           uint32_t flags) {
  siqs_factor_list_t pending = {NULL, 0, 0};
  mpz_t current;
  int complete = 1;

  mpz_init(current);
  append_factor(&pending, n);
  while (pending.count != 0) {
    mpz_t *partition;
    uint32_t count, i;

    mpz_swap(current, pending.values[--pending.count]);
    mpz_clear(pending.values[pending.count]);
#ifdef DMSIQS
    partition = dmsiqs_factor(&distributed_host, current, &count, 2,
                              verbose_level, nthreads, flags, journal);
#else
    if (journal != NULL)
      partition = gmp_siqs_journal_factor(journal, current, &count, 2,
                                          verbose_level, nthreads, flags);
    else
      partition = gmp_siqs(current, &count, 2, verbose_level, nthreads, flags);
#endif
    for (i = 0; i < count; i++) {
      if (mpz_probab_prime_p(partition[i], 15)) {
        append_factor(output, partition[i]);
      } else if (mpz_cmp_ui(partition[i], 1) > 0 &&
                 mpz_cmp(partition[i], current) < 0) {
        append_factor(&pending, partition[i]);
      } else {
        append_factor(output, partition[i]);
        complete = 0;
      }
    }
    gmp_siqs_free(partition, count);
#ifdef DMSIQS
    if (journal != NULL && distributed_host.failed) {
      /* Resume the interrupted cofactor before processing the others. */
      complete = 0;
      while (pending.count != 0) {
        --pending.count;
        append_factor(output, pending.values[pending.count]);
        mpz_clear(pending.values[pending.count]);
      }
      break;
    }
#endif
  }
  mpz_clear(current);
  free(pending.values);
  return complete;
}

static void sort_factors(siqs_factor_list_t *output) {
  size_t i, j;
  for (i = 1; i < output->count; i++) {
    for (j = i; j > 0 &&
         mpz_cmp(output->values[j], output->values[j - 1]) < 0; j--)
      mpz_swap(output->values[j], output->values[j - 1]);
  }
}

/* Takes ownership of the optional journal; finish validation before output. */
static int factor_number(const mpz_t n, uint32_t nthreads, siqs_journal_t *journal,
                         uint32_t flags) {
  siqs_factor_list_t output = {NULL, 0, 0};
  size_t i;
  int complete;

  if (mpz_sgn(n) <= 0) {
    gmp_fprintf(stderr, SIQS_PROGRAM_NAME ": input must be positive: %Zd\n", n);
    return 1;
  }
  if (mpz_cmp_ui(n, 1) == 0) {
    if (journal != NULL) {
      uint32_t count;
      mpz_t *values = gmp_siqs_journal_factor(journal, n, &count, 2,
                                              verbose_level, nthreads, flags);
      gmp_siqs_free(values, count);
      gmp_siqs_journal_finish(journal, NULL, 0, 1);
      gmp_siqs_journal_close(journal);
    }
    puts("1:");
    return 0;
  }

  complete = collect_factors(n, &output, nthreads, journal, flags);
#ifdef DMSIQS
  dmsiqs_close(&distributed_host, journal, verbose_level);
#endif
  sort_factors(&output);
  if (journal != NULL) {
#ifdef DMSIQS
    if (!distributed_host.failed)
#endif
      gmp_siqs_journal_finish(journal, output.values, output.count, complete);
    gmp_siqs_journal_close(journal);
  }
  /* Print only after processing every cofactor; SIQS's verbose lines therefore
   * cannot interrupt the final result line. */
  gmp_printf("%Zd:", n);
  for (i = 0; i < output.count; i++)
    gmp_printf(" %Zd", output.values[i]);
  if (!complete)
    printf(" [incomplete]");
  putchar('\n');
  fflush(stdout);
  for (i = 0; i < output.count; i++)
    mpz_clear(output.values[i]);
  free(output.values);

  if (!complete) {
    gmp_fprintf(stderr, SIQS_PROGRAM_NAME ": incomplete factorization of %Zd\n", n);
    return 1;
  }
  return 0;
}

static int factor_journal(const char *path, const char *input, uint32_t nthreads,
                          uint32_t flags) {
  siqs_journal_t *journal;
  mpz_t n;
  int ch, status = 2;

  mpz_init(n);
  if (input != NULL && strcmp(input, "-") == 0) {
    if (gmp_scanf("%Zd", n) != 1) {
      fputs(SIQS_PROGRAM_NAME ": --journal requires one input on stdin\n", stderr);
      goto done;
    }
    do { ch = getchar(); } while (ch != EOF && isspace((unsigned char)ch));
    if (ch != EOF || ferror(stdin)) {
      fputs(SIQS_PROGRAM_NAME ": --journal accepts exactly one input\n", stderr);
      goto done;
    }
  } else if (input != NULL && mpz_set_str(n, input, 10) != 0) {
    fputs(SIQS_PROGRAM_NAME ": --journal expects a decimal input or -\n", stderr);
    goto done;
  }
  if (input != NULL && mpz_sgn(n) <= 0) {
    status = factor_number(n, nthreads, NULL, flags);
  } else {
    journal = gmp_siqs_journal_open(path, input != NULL ? n : NULL);
    gmp_siqs_journal_input(journal, n);
    status = factor_number(n, nthreads, journal, flags);
  }
done:
  mpz_clear(n);
  return status;
}

#ifdef DMSIQS
# include "dmsiqs_cli.inc.c"
#else
int main(int argc, char **argv) {
  mpz_t n;
  int argument = 1, status = 0;
  uint32_t nthreads = 1;
  uint32_t flags = 0;
  const char *journal_path = NULL;

  while (argument < argc && argv[argument][0] == '-') {
    const char *option = argv[argument];
    if (strcmp(option, "--") == 0) {
      argument++;
      break;
    }
    if (journal_path != NULL && strcmp(option, "-") == 0)
      break;
    if (strcmp(option, "-h") == 0 || strcmp(option, "--help") == 0) {
      print_usage(stdout, argv[0]);
      return 0;
    }
    if (strcmp(option, "--2lp") == 0 || strcmp(option, "--3lp") == 0) {
      uint32_t selection = strcmp(option, "--2lp") == 0
                         ? SIQS_OPTION_2LP : SIQS_OPTION_3LP;
      if (flags != 0 && flags != selection) {
        fprintf(stderr, SIQS_PROGRAM_NAME ": --2lp and --3lp are mutually exclusive\n");
        return 2;
      }
      flags = selection;
      argument++;
      continue;
    }
    if (strcmp(option, "--journal") == 0) {
      if (journal_path != NULL || argument + 1 == argc || argv[argument + 1][0] == '\0') {
        fprintf(stderr, SIQS_PROGRAM_NAME ": --journal requires one nonempty filename\n");
        return 2;
      }
      journal_path = argv[argument + 1];
      argument += 2;
      continue;
    }
#ifdef PSIQS
    if (strcmp(option, "-t") == 0 ||
        strcmp(option, "-threads") == 0 ||
        strcmp(option, "--threads") == 0) {
      if (argument + 1 == argc ||
          !parse_thread_count(argv[argument + 1], &nthreads)) {
        fprintf(stderr, SIQS_PROGRAM_NAME ": %s requires a worker count "
                "between 1 and %u\n", option, (unsigned)PSIQS_MAX_THREADS);
        return 2;
      }
      argument += 2;
      continue;
    }
#endif
    if (strcmp(option, "--verbose") == 0) {
      verbose_level++;
    } else if (option[1] == 'v' && option[2] != '\0') {
      const char *p;
      for (p = option + 1; *p == 'v'; p++)
        verbose_level++;
      if (*p != '\0') {
        fprintf(stderr, SIQS_PROGRAM_NAME ": unknown option: %s\n", option);
        print_usage(stderr, argv[0]);
        return 2;
      }
    } else if (strcmp(option, "-v") == 0) {
      verbose_level++;
    } else {
      fprintf(stderr, SIQS_PROGRAM_NAME ": unknown option: %s\n", option);
      print_usage(stderr, argv[0]);
      return 2;
    }
    argument++;
  }


  if (journal_path != NULL) {
    if (argc - argument > 1) {
      fprintf(stderr, SIQS_PROGRAM_NAME ": --journal accepts only one input\n");
      return 2;
    }
    return factor_journal(journal_path, argument < argc ? argv[argument] : NULL,
                          nthreads, flags);
  }

  mpz_init(n);
  if (argument < argc) {
    for (; argument < argc; argument++) {
      if (mpz_set_str(n, argv[argument], 10) != 0) {
        fprintf(stderr, SIQS_PROGRAM_NAME ": invalid decimal integer: %s\n",
                argv[argument]);
        status = 2;
        continue;
      }
      if (factor_number(n, nthreads, NULL, flags) != 0 && status == 0)
        status = 1;
    }
  } else {
    int scanned;
    while ((scanned = gmp_scanf("%Zd", n)) == 1)
      if (factor_number(n, nthreads, NULL, flags) != 0 && status == 0)
        status = 1;
    if (scanned != EOF) {
      fprintf(stderr, SIQS_PROGRAM_NAME ": invalid input on stdin\n");
      status = 2;
    }
  }
  mpz_clear(n);

  return status;
}
#endif
