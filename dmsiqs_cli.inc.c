/* Distributed command-line options, using the standard factor output driver. */
#include <arpa/inet.h>
#include "dmsiqs_wire.h"

static int distributed_port_valid(const char *value) {
  uint32_t port = 0;
  const unsigned char *p = (const unsigned char *)value;
  while (*p >= '0' && *p <= '9') {
    port = 10U * port + (*p++ - '0');
    if (port > 65535U) return 0;
  }
  return !*p && port != 0;
}

static int distributed_wildcard(const char *address) {
  struct in6_addr ipv6;
  return strcmp(address, "0.0.0.0") == 0 ||
         (inet_pton(AF_INET6, address, &ipv6) == 1 && IN6_IS_ADDR_UNSPECIFIED(&ipv6));
}

int main(int argc, char **argv) {
  const char *bind_address = NULL, *port = NULL, *name = NULL, *input = NULL;
  const char *pool = NULL, *external_arg = NULL;
  const char *journal_path = NULL;
  char *coordinator = NULL, *listener = NULL, *external = NULL;
  const char **workers = calloc((size_t)argc, sizeof(*workers));
  uint32_t threads = 1, flags = 0;
  int worker_mode = 0, mode_seen = 0, worker_stall_seen = 0, worker_report_seen = 0;
  int positional = 0, i, status = 2;
  mpz_t n;
  if (workers == NULL) {
    fputs("dmsiqs: cannot allocate worker list\n", stderr);
    return 1;
  }
  distributed_host.endpoints = workers;
  distributed_host.worker_stall_seconds = DMSIQS_DEFAULT_WORKER_STALL_SECONDS;
  distributed_host.worker_report_seconds = DMSIQS_DEFAULT_WORKER_REPORT_SECONDS;
  for (i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    if (positional || arg[0] != '-' || !strcmp(arg, "-")) {
      if (input != NULL) goto usage;
      input = arg;
    } else if (!strcmp(arg, "--")) {
      positional = 1;
    } else if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
      print_usage(stdout, argv[0]);
      status = 0;
      goto done;
    } else if (!strcmp(arg, "--version")) {
      printf("dmsiqs version %u", DS_VERSION);
#ifdef DMSIQS_GIT_INFO
      printf(", %s", DMSIQS_GIT_INFO);
#endif
      putchar('\n');
      status = 0;
      goto done;
    } else if (!strcmp(arg, "--2lp") || !strcmp(arg, "--3lp")) {
      uint32_t selected = !strcmp(arg, "--3lp") ? SIQS_OPTION_3LP : SIQS_OPTION_2LP;
      if (flags != 0 && flags != selected) goto usage;
      flags = selected;
    } else if (!strcmp(arg, "--verbose")) {
      ++verbose_level;
    } else if (arg[1] == 'v' && arg[1 + strspn(arg + 1, "v")] == '\0') {
      verbose_level += (int)strlen(arg + 1);
    } else {
      const char *value;
      if (++i == argc || argv[i][0] == '\0') goto usage;
      value = argv[i];
      if (!strcmp(arg, "--worker")) {
        char *endpoint = ds_registry_endpoint(value, DS_WORKER_PORT);
        if (endpoint == NULL) goto usage;
        workers[distributed_host.count++] = endpoint;
      } else if (!strcmp(arg, "--journal") && journal_path == NULL) journal_path = value;
      else if (!strcmp(arg, "--worker-stall") && !worker_stall_seen) {
        if (!parse_uint32(value, &distributed_host.worker_stall_seconds)) goto usage;
        worker_stall_seen = 1;
      } else if (!strcmp(arg, "--worker-report") && !worker_report_seen) {
        if (!parse_uint32(value, &distributed_host.worker_report_seconds)) goto usage;
        worker_report_seen = 1;
      } else if ((!strcmp(arg, "--coord") || !strcmp(arg, "--coordinator")) && coordinator == NULL) {
        coordinator = ds_registry_endpoint(value, DS_COORDINATOR_PORT);
        if (coordinator == NULL) goto usage;
      } else if (!strcmp(arg, "--pool") && pool == NULL && ds_registry_name_valid(value)) {
        pool = value;
      } else if ((!strcmp(arg, "--external") || !strcmp(arg, "--advertise")) && external_arg == NULL) {
        external_arg = value;
      } else if (!strcmp(arg, "-t") || !strcmp(arg, "-threads") ||
               !strcmp(arg, "--threads")) {
        if (!parse_thread_count(value, &threads)) goto usage;
      } else if (!strcmp(arg, "--mode") && !mode_seen) {
        if (strcmp(value, "worker") && strcmp(value, "host")) goto usage;
        worker_mode = !strcmp(value, "worker");
        mode_seen = 1;
      } else if (!strcmp(arg, "--bind") && bind_address == NULL) bind_address = value;
      else if (!strcmp(arg, "--port") && port == NULL && distributed_port_valid(value)) port = value;
      else if (!strcmp(arg, "--name") && name == NULL && strlen(value) <= 127U) {
        const unsigned char *p = (const unsigned char *)value;
        while (*p && *p >= 32U && *p != 127U) ++p;
        if (*p) goto usage;
        name = value;
      } else goto usage;
    }
  }
  if ((pool || external_arg) && coordinator == NULL) goto usage;
  if (pool == NULL) pool = DS_DEFAULT_POOL;
  if (worker_mode) {
    char *port_separator;
    if (input || distributed_host.count || flags || journal_path ||
        worker_stall_seen || worker_report_seen) goto usage;
    listener = ds_registry_endpoint(bind_address ? bind_address : "127.0.0.1",
                                    port ? port : DS_WORKER_PORT);
    if (listener == NULL) goto usage;
    port_separator = strrchr(listener, ':');
    if (port != NULL && strcmp(port, port_separator + 1) != 0) {
      fputs("dmsiqs: conflicting ports in --bind and --port\n", stderr);
      goto usage;
    }
    port = port_separator + 1;
    if (coordinator != NULL) {
      external = ds_registry_endpoint(external_arg != NULL ? external_arg : listener, port);
      if (external == NULL) goto usage;
    }
    *port_separator = '\0';
    bind_address = listener;
    if (listener[0] == '[') {
      bind_address = listener + 1;
      port_separator[-1] = '\0';
    }
    if (coordinator && !external_arg && distributed_wildcard(bind_address)) {
      fputs("dmsiqs: --external is required with a wildcard bind and a coordinator\n", stderr);
      goto usage;
    }
    status = dmsiqs_worker(bind_address, port, threads, name, coordinator, pool, external, verbose_level);
  } else {
    if ((input == NULL && journal_path == NULL) ||
        (distributed_host.count == 0 && coordinator == NULL) ||
        bind_address || port || name || external_arg)
      goto usage;
    distributed_host.coordinator = coordinator;
    distributed_host.pool = pool;
    if (journal_path != NULL) {
      status = factor_journal(journal_path, input, threads, flags);
    } else {
      mpz_init(n);
      if (mpz_set_str(n, input, 10) != 0) {
        fputs("dmsiqs: expected a positive decimal input\n", stderr);
      } else {
        status = factor_number(n, threads, NULL, flags);
      }
      mpz_clear(n);
    }
  }
  goto done;
usage:
  print_usage(stderr, argv[0]);
done:
  free(external);
  free(listener);
  free(coordinator);
  while (distributed_host.count != 0)
    free((void *)workers[--distributed_host.count]);
  free(workers);
  return status;
}
