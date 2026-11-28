#include "dmsiqs_registry.h"
#include "dmsiqs_wire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int ds_registry_name_valid(const char *name) {
  const unsigned char *c = (const unsigned char *)name;
  if (c == NULL || *c == '\0' || strlen(name) > 127U) return 0;
  for (; *c != '\0'; ++c)
    if (*c < 32U || *c == 127U) return 0;
  return 1;
}

char *ds_registry_endpoint(const char *input, const char *default_port) {
  const char *begin = input, *end, *port = default_port, *c;
  unsigned number = 0;
  size_t length, size;
  int bracketed = 0;
  char *endpoint;
  if (input == NULL || *input == '\0') return NULL;
  if (*begin == '[') {
    bracketed = 1;
    end = strchr(++begin, ']');
    if (end == NULL || (end[1] != '\0' && end[1] != ':')) return NULL;
    if (end[1] == ':') port = end + 2;
  } else {
    end = strchr(begin, ':');
    if (end != NULL && strchr(end + 1, ':') == NULL) {
      port = end + 1;
    } else {
      bracketed = end != NULL;
      end = begin + strlen(begin);
    }
  }
  length = (size_t)(end - begin);
  if (length == 0 || length > DS_REG_MAX_ENDPOINT || port == NULL) return NULL;
  for (c = begin; c != end; ++c)
    if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
          (*c >= '0' && *c <= '9') || *c == '.' || *c == '-' || *c == '_' ||
          (bracketed && (*c == ':' || *c == '%')))) return NULL;
  for (c = port; *c >= '0' && *c <= '9'; ++c) {
    number = number * 10U + (unsigned)(*c - '0');
    if (number > 65535U) return NULL;
  }
  if (*c != '\0' || number == 0 || strlen(port) > 5U) return NULL;
  size = length + strlen(port) + (bracketed ? 3U : 1U);
  if (size > DS_REG_MAX_ENDPOINT) return NULL;
  endpoint = malloc(size + 1U);
  if (endpoint != NULL)
    snprintf(endpoint, size + 1U, bracketed ? "[%.*s]:%s" : "%.*s:%s", (int)length, begin, port);
  return endpoint;
}

static void ds_registry_put_name(ds_buf *request, const char *name) {
  size_t length = strlen(name);
  ds_put_u8(request, (uint8_t)length);
  ds_put_bytes(request, name, length);
}

static int ds_registry_exchange(const char *coordinator, unsigned request_type,
                                const ds_buf *request, ds_frame *reply) {
  ds_peer *peer;
  int fd, received;
  if (request->bad || (fd = ds_connect(coordinator, 1000)) < 0) return 0;
  peer = ds_peer_open(fd);
  if (peer == NULL) return 0;
  received = ds_peer_send(peer, request_type, request) && ds_peer_recv(peer, reply, 2000) == 1;
  ds_peer_close(peer);
  return received;
}

int ds_registry_register(const char *coordinator, const char *pool,
                         const char *worker_id, const char *endpoint) {
  ds_buf request = {0};
  ds_frame reply = {0};
  int okay;
  if (!ds_registry_name_valid(pool) || !ds_registry_name_valid(worker_id)) return 0;
  ds_registry_put_name(&request, pool);
  ds_registry_put_name(&request, worker_id);
  ds_put_u32(&request, (uint32_t)strlen(endpoint));
  ds_put_bytes(&request, endpoint, strlen(endpoint));
  okay = ds_registry_exchange(coordinator, DS_REG_REGISTER, &request, &reply) &&
         reply.type == DS_REG_REGISTERED && reply.size == 0;
  ds_frame_clear(&reply);
  ds_buf_clear(&request);
  return okay;
}

void ds_registry_free(char **entries, size_t count) {
  size_t i;
  for (i = 0; i < count; ++i) free(entries[i]);
  free(entries);
}

int ds_registry_report_stall(const char *coordinator, const char *pool,
                             const char *endpoint) {
  ds_buf request = {0};
  ds_frame reply = {0};
  size_t length = strlen(endpoint);
  int answered;
  if (!ds_registry_name_valid(pool) || length == 0 || length > DS_REG_MAX_ENDPOINT)
    return 0;
  ds_registry_put_name(&request, pool);
  ds_put_u32(&request, (uint32_t)length);
  ds_put_bytes(&request, endpoint, length);
  /* An older directory can reject the advisory without affecting discovery. */
  answered = ds_registry_exchange(coordinator, DS_REG_REPORT_STALL, &request, &reply) &&
             ((reply.type == DS_REG_REGISTERED && reply.size == 0) ||
              reply.type == DS_ERROR);
  ds_frame_clear(&reply);
  ds_buf_clear(&request);
  return answered;
}

static int ds_registry_decode(const ds_frame *reply, char ***entries, size_t *count) {
  ds_reader reader = {reply->data, reply->size, 0};
  uint32_t n = ds_get_u32(&reader), i;
  char **list = NULL;
  if (reader.bad || n > DS_REG_MAX_WORKERS || n > reader.left / 5U) return 0;
  if (n != 0) {
    list = calloc(n, sizeof(*list));
    if (list == NULL) return 0;
  }
  for (i = 0; i < n; ++i) {
    uint32_t length = ds_get_u32(&reader);
    const unsigned char *data = ds_get_bytes(&reader, length);
    char input[DS_REG_MAX_ENDPOINT + 1U];
    if (reader.bad || length == 0 || length > DS_REG_MAX_ENDPOINT || memchr(data, 0, length)) break;
    memcpy(input, data, length);
    input[length] = '\0';
    list[i] = ds_registry_endpoint(input, NULL);
    if (list[i] == NULL) break;
  }
  if (i != n || !ds_reader_done(&reader)) {
    ds_registry_free(list, n);
    return 0;
  }
  *entries = list;
  *count = n;
  return 1;
}

int ds_registry_list(const char *coordinator, const char *pool, char ***entries, size_t *count) {
  ds_buf request = {0};
  ds_frame reply = {0};
  int okay;
  *entries = NULL;
  *count = 0;
  if (!ds_registry_name_valid(pool)) return 0;
  ds_registry_put_name(&request, pool);
  okay = ds_registry_exchange(coordinator, DS_REG_LIST, &request, &reply) &&
         reply.type == DS_REG_LISTED && ds_registry_decode(&reply, entries, count);
  ds_frame_clear(&reply);
  ds_buf_clear(&request);
  return okay;
}
