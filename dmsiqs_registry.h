#ifndef DMSIQS_REGISTRY_H
#define DMSIQS_REGISTRY_H

#include <stddef.h>

#define DS_COORDINATOR_PORT "8013"
#define DS_WORKER_PORT "8012"
#define DS_DEFAULT_POOL "default"

int ds_registry_name_valid(const char *);
/* Return an owned address with its port, or NULL for an invalid address. */
char *ds_registry_endpoint(const char *, const char *default_port);
int ds_registry_register(const char *, const char *, const char *, const char *);
int ds_registry_report_stall(const char *, const char *, const char *);
int ds_registry_list(const char *, const char *, char ***, size_t *);
void ds_registry_free(char **, size_t);

#endif
