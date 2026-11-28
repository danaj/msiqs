#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
# define _DARWIN_C_SOURCE 1
#endif
#include "dmsiqs_wire.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DS_HEADER_SIZE 12U
#define DS_PING_SECONDS 5.0
#define DS_DEAD_SECONDS 30.0

typedef struct ds_node {
  struct ds_node *next;
  ds_frame frame;
} ds_node;

typedef struct {
  ds_node *head, *tail;
  size_t bytes;
} ds_queue;

struct ds_peer {
  int fd, wake[2];
  int alive, stopping;
  pthread_t thread;
  pthread_mutex_t lock;
  pthread_cond_t changed;
  ds_queue input, output;
  size_t input_high_water;
  /* Only the I/O thread touches these offsets and the partial frame. */
  size_t written, header_read, body_read;
  unsigned char header[DS_HEADER_SIZE];
  ds_frame incoming;
};

static void store_u32(unsigned char *p, uint32_t value) {
  p[0] = (unsigned char)(value >> 24);
  p[1] = (unsigned char)(value >> 16);
  p[2] = (unsigned char)(value >> 8);
  p[3] = (unsigned char)value;
}

static uint32_t load_u32(const unsigned char *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
         (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

void ds_put_bytes(ds_buf *b, const void *data, size_t size) {
  if (b->bad || size == 0) return;
  if (data == NULL || size > DS_SETUP_MAX || b->size > DS_SETUP_MAX - size) {
    b->bad = 1;
    return;
  }
  if (b->alloc < b->size + size) {
    size_t alloc = b->alloc ? b->alloc : 256;
    unsigned char *new_data;
    while (alloc < b->size + size) alloc *= 2;
    new_data = realloc(b->data, alloc);
    if (new_data == NULL) {
      b->bad = 1;
      return;
    }
    b->data = new_data;
    b->alloc = alloc;
  }
  memcpy(b->data + b->size, data, size);
  b->size += size;
}

void ds_put_u8(ds_buf *b, uint8_t value) { ds_put_bytes(b, &value, 1); }

void ds_put_u32(ds_buf *b, uint32_t value) {
  unsigned char p[4];
  store_u32(p, value);
  ds_put_bytes(b, p, sizeof(p));
}

void ds_put_u64(ds_buf *b, uint64_t value) {
  unsigned char p[8];
  store_u32(p, (uint32_t)(value >> 32));
  store_u32(p + 4, (uint32_t)value);
  ds_put_bytes(b, p, sizeof(p));
}

void ds_buf_clear(ds_buf *b) {
  free(b->data);
  memset(b, 0, sizeof(*b));
}

const unsigned char *ds_get_bytes(ds_reader *r, size_t size) {
  const unsigned char *data;
  if (r->bad || size > r->left) {
    r->bad = 1;
    return NULL;
  }
  data = r->data;
  if (size != 0) r->data += size;
  r->left -= size;
  return data;
}

uint8_t ds_get_u8(ds_reader *r) {
  const unsigned char *p = ds_get_bytes(r, 1);
  return p ? *p : 0;
}

uint32_t ds_get_u32(ds_reader *r) {
  const unsigned char *p = ds_get_bytes(r, 4);
  return p ? load_u32(p) : 0;
}

uint64_t ds_get_u64(ds_reader *r) {
  const unsigned char *p = ds_get_bytes(r, 8);
  return p ? (uint64_t)load_u32(p) << 32 | load_u32(p + 4) : 0;
}

int ds_reader_done(const ds_reader *r) { return !r->bad && r->left == 0; }

void ds_frame_clear(ds_frame *frame) {
  free(frame->data);
  memset(frame, 0, sizeof(*frame));
}

double ds_now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

int ds_process_usage(ds_cpu_usage *usage, uint64_t *peak_rss_bytes) {
  struct rusage measured;
  if (peak_rss_bytes != NULL) *peak_rss_bytes = 0;
  if (getrusage(RUSAGE_SELF, &measured) != 0) return 0;
  if (usage != NULL) {
    usage->user_us = (uint64_t)measured.ru_utime.tv_sec * 1000000U + measured.ru_utime.tv_usec;
    usage->system_us = (uint64_t)measured.ru_stime.tv_sec * 1000000U + measured.ru_stime.tv_usec;
  }
#if defined(__APPLE__) || defined(__linux__)
  if (peak_rss_bytes != NULL && measured.ru_maxrss > 0) {
    uint64_t peak = (uint64_t)measured.ru_maxrss;
# if defined(__linux__)
    /* Linux reports KiB. Darwin already reports bytes. */
    if (peak <= UINT64_MAX / 1024U) *peak_rss_bytes = peak * 1024U;
# else
    *peak_rss_bytes = peak;
# endif
  }
#endif
  return 1;
}

int ds_process_cpu(ds_cpu_usage *usage) {
  return ds_process_usage(usage, NULL);
}

static struct timespec realtime_deadline(unsigned milliseconds) {
  struct timespec t;
  clock_gettime(CLOCK_REALTIME, &t);
  t.tv_sec += milliseconds / 1000;
  t.tv_nsec += (long)(milliseconds % 1000) * 1000000L;
  if (t.tv_nsec >= 1000000000L) {
    t.tv_sec++;
    t.tv_nsec -= 1000000000L;
  }
  return t;
}

static int prepare_fd(int fd, int nonblocking) {
  int fd_flags = fcntl(fd, F_GETFD);
  if (fd_flags < 0 || fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC) < 0)
    return 0;
  if (nonblocking) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
      return 0;
  }
  return 1;
}

static int valid_port(const char *service, int allow_zero) {
  unsigned port = 0;
  if (service == NULL || *service == '\0') return 0;
  for (; *service != '\0'; ++service) {
    if (*service < '0' || *service > '9') return 0;
    port = port * 10 + (unsigned)(*service - '0');
    if (port > 65535U) return 0;
  }
  return port != 0 || allow_zero;
}

int ds_listen(const char *address, const char *port) {
  struct addrinfo hints, *list, *item;
  int fd = -1, error = EADDRNOTAVAIL;
  if (!valid_port(port, 1)) { errno = EINVAL; return -1; }
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICSERV;
  if (getaddrinfo(address ? address : "127.0.0.1", port, &hints, &list) != 0) {
    errno = EADDRNOTAVAIL;
    return -1;
  }
  for (item = list; item != NULL; item = item->ai_next) {
    int one = 1;
    fd = socket(item->ai_family, item->ai_socktype, item->ai_protocol);
    if (fd < 0) { error = errno; continue; }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (prepare_fd(fd, 0) && bind(fd, item->ai_addr, item->ai_addrlen) == 0 &&
        listen(fd, 32) == 0)
      break;
    error = errno;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(list);
  if (fd < 0) errno = error;
  return fd;
}

static int connect_ready(int fd, double deadline) {
  struct pollfd p = {fd, POLLOUT, 0};
  for (;;) {
    double remaining = (deadline - ds_now()) * 1000;
    int wait = remaining > INT_MAX - 1 ? INT_MAX :
               remaining > 0 ? (int)remaining + 1 : 0;
    int ready = poll(&p, 1, wait);
    if (ready > 0) {
      int status;
      socklen_t size = sizeof(status);
      if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &status, &size) < 0) return 0;
      if (status) { errno = status; return 0; }
      return 1;
    }
    if (ready == 0) { errno = ETIMEDOUT; return 0; }
    if (errno != EINTR) return 0;
  }
}

int ds_connect(const char *endpoint, unsigned timeout_ms) {
  struct addrinfo hints, *list, *item;
  const char *start = endpoint, *end, *port;
  char address[1025];
  int fd = -1, error = EHOSTUNREACH;
  double deadline;
  if (endpoint == NULL) { errno = EINVAL; return -1; }
  if (*start == '[') {
    end = strchr(++start, ']');
    if (end == NULL || end[1] != ':') { errno = EINVAL; return -1; }
    port = end + 2;
  } else {
    end = strchr(start, ':');
    if (end == NULL) { errno = EINVAL; return -1; }
    port = end + 1;
  }
  if (end == start || (size_t)(end - start) >= sizeof(address) ||
      !valid_port(port, 0)) { errno = EINVAL; return -1; }
  memcpy(address, start, (size_t)(end - start));
  address[end - start] = '\0';
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICSERV;
  if (getaddrinfo(address, port, &hints, &list) != 0) {
    errno = EHOSTUNREACH;
    return -1;
  }
  deadline = ds_now() + (double)timeout_ms / 1000;
  for (item = list; item != NULL; item = item->ai_next) {
    fd = socket(item->ai_family, item->ai_socktype, item->ai_protocol);
    if (fd < 0) { error = errno; continue; }
    if (prepare_fd(fd, 1) &&
        (connect(fd, item->ai_addr, item->ai_addrlen) == 0 ||
         ((errno == EINPROGRESS || errno == EINTR) && connect_ready(fd, deadline))))
      break;
    error = errno;
    close(fd);
    fd = -1;
    if (ds_now() >= deadline) break;
  }
  freeaddrinfo(list);
  if (fd < 0) errno = error;
  return fd;
}

static size_t frame_limit(unsigned type) {
  if (type == DS_PING || type == DS_PONG || type == DS_REG_REGISTERED ||
      type == DS_REG_STALLED) return 0;
  if (type == DS_REG_REGISTER) return 2048U;
  if (type == DS_REG_LIST) return 128U;
  if (type == DS_REG_LISTED) return 4U + DS_REG_MAX_WORKERS * (4U + DS_REG_MAX_ENDPOINT);
  return type == DS_SETUP ? DS_SETUP_MAX : DS_FRAME_MAX;
}

static int valid_frame(unsigned type, size_t size) {
  return type >= DS_CLAIM && type <= DS_REG_REPAIRED && size <= frame_limit(type);
}

static void enqueue(ds_queue *q, ds_node *node) {
  node->next = NULL;
  if (q->tail) q->tail->next = node;
  else q->head = node;
  q->tail = node;
  q->bytes += sizeof(*node) + node->frame.size;
}

static ds_node *dequeue(ds_queue *q) {
  ds_node *node = q->head;
  if (node != NULL) {
    q->head = node->next;
    if (q->head == NULL) q->tail = NULL;
    q->bytes -= sizeof(*node) + node->frame.size;
  }
  return node;
}

static void clear_queue(ds_queue *q) {
  ds_node *node;
  while ((node = dequeue(q)) != NULL) {
    ds_frame_clear(&node->frame);
    free(node);
  }
}

static void wake_io(ds_peer *p) {
  unsigned char marker = 0;
  ssize_t amount;
  do { amount = write(p->wake[1], &marker, 1); }
  while (amount < 0 && errno == EINTR);
  /* A full pipe already has a pending wakeup. */
}

/* The caller holds the peer lock. Outgoing nodes include their wire header. */
static int queue_output(ds_peer *p, unsigned type,
                        const unsigned char *data, size_t size) {
  ds_node *node;
  unsigned char *header;
  if (!p->alive || p->stopping ||
      p->output.bytes + sizeof(*node) + DS_HEADER_SIZE + size > DS_QUEUE_MAX)
    return 0;
  node = malloc(sizeof(*node));
  if (node == NULL) return 0;
  header = malloc(DS_HEADER_SIZE + size);
  if (header == NULL) { free(node); return 0; }
  memcpy(header, "DMSQ", 4);
  header[4] = 0;
  header[5] = DS_VERSION;
  header[6] = (unsigned char)(type >> 8);
  header[7] = (unsigned char)type;
  store_u32(header + 8, (uint32_t)size);
  if (size != 0) memcpy(header + DS_HEADER_SIZE, data, size);
  node->frame.type = type;
  node->frame.size = DS_HEADER_SIZE + size;
  node->frame.data = header;
  enqueue(&p->output, node);
  return 1;
}

static int temporary_io_error(void) {
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

static int write_frame(ds_peer *p) {
  ds_node *node;
  ssize_t n;
  int flags = 0, ok = 1;
#ifdef MSG_NOSIGNAL
  flags |= MSG_NOSIGNAL;
#endif
  pthread_mutex_lock(&p->lock);
  node = p->output.head;
  if (node != NULL) {
    n = send(p->fd, node->frame.data + p->written,
             node->frame.size - p->written, flags);
    if (n > 0) {
      p->written += (size_t)n;
      if (p->written == node->frame.size) {
        dequeue(&p->output);
        ds_frame_clear(&node->frame);
        free(node);
        p->written = 0;
        pthread_cond_broadcast(&p->changed);
      }
    } else if (n == 0 || !temporary_io_error()) {
      ok = 0;
    }
  }
  pthread_mutex_unlock(&p->lock);
  return ok;
}

/* Called with the lock held. A partial body always has queue space reserved
 * since this I/O thread is the only producer of input. */
static int has_input_room(const ds_peer *p) {
  return p->input.bytes + sizeof(ds_node) + p->incoming.size <= DS_QUEUE_MAX;
}

static int publish_input(ds_peer *p) {
  ds_node *node = NULL;
  int ok = 1;
  pthread_mutex_lock(&p->lock);
  if (p->incoming.type == DS_PING) {
    /* A loaded connection already has outbound traffic for liveness. */
    (void)queue_output(p, DS_PONG, NULL, 0);
  } else if (p->incoming.type != DS_PONG) {
    node = malloc(sizeof(*node));
    if (node != NULL) {
      node->frame = p->incoming;
      p->incoming.data = NULL;
      enqueue(&p->input, node);
      if (p->input.bytes > p->input_high_water)
        p->input_high_water = p->input.bytes;
      pthread_cond_broadcast(&p->changed);
    } else {
      ok = 0;
    }
  }
  pthread_mutex_unlock(&p->lock);
  ds_frame_clear(&p->incoming);
  p->header_read = p->body_read = 0;
  return ok;
}

static int read_frame(ds_peer *p, double *last_rx) {
  ssize_t n;
  if (p->header_read < DS_HEADER_SIZE) {
    n = recv(p->fd, p->header + p->header_read, DS_HEADER_SIZE - p->header_read, 0);
    if (n <= 0) return n < 0 && temporary_io_error();
    *last_rx = ds_now();
    p->header_read += (size_t)n;
    if (p->header_read < DS_HEADER_SIZE) return 1;
    if (memcmp(p->header, "DMSQ", 4) != 0 ||
        p->header[4] != 0 || p->header[5] != DS_VERSION)
      return 0;
    p->incoming.type = (unsigned)p->header[6] << 8 | p->header[7];
    p->incoming.size = load_u32(p->header + 8);
    if (!valid_frame(p->incoming.type, p->incoming.size)) return 0;
  }
  if (p->incoming.size == 0) return publish_input(p);
  if (p->incoming.data == NULL) {
    int room;
    pthread_mutex_lock(&p->lock);
    room = has_input_room(p);
    pthread_mutex_unlock(&p->lock);
    if (!room) return 1;
    p->incoming.data = malloc(p->incoming.size);
    if (p->incoming.data == NULL) return 0;
  }
  n = recv(p->fd, p->incoming.data + p->body_read,
           p->incoming.size - p->body_read, 0);
  if (n <= 0) return n < 0 && temporary_io_error();
  *last_rx = ds_now();
  p->body_read += (size_t)n;
  return p->body_read < p->incoming.size || publish_input(p);
}

static void *peer_io(void *argument) {
  ds_peer *p = argument;
  double last_rx = ds_now(), last_ping = last_rx;
  for (;;) {
    struct pollfd pollers[2] = {{p->fd, 0, 0}, {p->wake[0], POLLIN, 0}};
    int room, stopping, ready;
    double now = ds_now();
    pthread_mutex_lock(&p->lock);
    stopping = p->stopping;
    room = has_input_room(p);
    if (now - last_ping >= DS_PING_SECONDS) {
      (void)queue_output(p, DS_PING, NULL, 0);
      last_ping = now;
    }
    if (room) pollers[0].events |= POLLIN;
    if (p->output.head) pollers[0].events |= POLLOUT;
    pthread_mutex_unlock(&p->lock);
    if (stopping) break;
    if (!room) last_rx = now; /* Do not time out while the consumer is backlogged. */
    if (now - last_rx > DS_DEAD_SECONDS) break;
    if (!pollers[0].events) pollers[0].fd = -1;
    ready = poll(pollers, 2, 1000);
    if (ready < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (pollers[1].revents & POLLIN) {
      unsigned char discard[128];
      while (read(p->wake[0], discard, sizeof(discard)) > 0) {}
    }
    if (room && (pollers[0].revents & (POLLIN | POLLHUP)) && !read_frame(p, &last_rx))
      break;
    if (pollers[0].revents & POLLOUT)
      if (!write_frame(p)) break;
    if (pollers[0].revents & (POLLERR | POLLNVAL)) break;
    if (!room && (pollers[0].revents & POLLHUP)) break;
  }
  /* Close the stream in both directions promptly on a local transport error.
   * Retain complete input frames for the caller to drain. */
  shutdown(p->fd, SHUT_RDWR);
  pthread_mutex_lock(&p->lock);
  p->alive = 0;
  pthread_cond_broadcast(&p->changed);
  pthread_mutex_unlock(&p->lock);
  return NULL;
}

ds_peer *ds_peer_open(int fd) {
  ds_peer *p;
  int one = 1;
  if (fd < 0) return NULL;
  if (!prepare_fd(fd, 1)) { close(fd); return NULL; }
#ifdef SO_NOSIGPIPE
  if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) != 0) {
    close(fd);
    return NULL;
  }
#endif
  (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  p = calloc(1, sizeof(*p));
  if (p == NULL) { close(fd); return NULL; }
  p->fd = fd;
  if (pipe(p->wake) < 0) goto fail;
  if (!prepare_fd(p->wake[0], 1) || !prepare_fd(p->wake[1], 1)) goto fail_pipe;
  if (pthread_mutex_init(&p->lock, NULL) != 0) goto fail_pipe;
  if (pthread_cond_init(&p->changed, NULL) != 0) goto fail_mutex;
  p->alive = 1;
  if (pthread_create(&p->thread, NULL, peer_io, p) != 0) goto fail_cond;
  return p;

fail_cond:
  pthread_cond_destroy(&p->changed);
fail_mutex:
  pthread_mutex_destroy(&p->lock);
fail_pipe:
  close(p->wake[0]);
  close(p->wake[1]);
fail:
  close(fd);
  free(p);
  return NULL;
}

int ds_peer_send(ds_peer *p, unsigned type, const ds_buf *b) {
  size_t size = b != NULL ? b->size : 0;
  int ok;
  if ((b != NULL && (b->bad || (size != 0 && !b->data))) ||
      !valid_frame(type, size)) return 0;
  pthread_mutex_lock(&p->lock);
  ok = queue_output(p, type, b != NULL ? b->data : NULL, size);
  pthread_mutex_unlock(&p->lock);
  if (ok) wake_io(p);
  return ok;
}

int ds_peer_recv(ds_peer *p, ds_frame *frame, unsigned timeout_ms) {
  struct timespec deadline;
  ds_node *node;
  int alive, backlogged;
  if (timeout_ms) deadline = realtime_deadline(timeout_ms);
  pthread_mutex_lock(&p->lock);
  while (!p->input.head && p->alive && timeout_ms) {
    if (pthread_cond_timedwait(&p->changed, &p->lock, &deadline) != 0) break;
  }
  backlogged = p->input.bytes > DS_QUEUE_MAX - DS_SETUP_MAX - sizeof(ds_node);
  node = dequeue(&p->input);
  alive = p->alive;
  pthread_mutex_unlock(&p->lock);
  if (node == NULL) return alive ? 0 : -1;
  *frame = node->frame;
  free(node);
  if (backlogged) wake_io(p);
  return 1;
}

size_t ds_peer_input_high_water(ds_peer *p) {
  size_t bytes;
  pthread_mutex_lock(&p->lock);
  bytes = p->input_high_water;
  pthread_mutex_unlock(&p->lock);
  return bytes;
}

int ds_peer_alive(ds_peer *p) {
  int alive;
  pthread_mutex_lock(&p->lock);
  alive = p->alive;
  pthread_mutex_unlock(&p->lock);
  return alive;
}

int ds_peer_flush(ds_peer *p, unsigned timeout_ms) {
  struct timespec deadline;
  int flushed;
  if (timeout_ms) deadline = realtime_deadline(timeout_ms);
  pthread_mutex_lock(&p->lock);
  while (p->output.head && p->alive && timeout_ms) {
    if (pthread_cond_timedwait(&p->changed, &p->lock, &deadline) != 0) break;
  }
  flushed = p->output.head == NULL;
  pthread_mutex_unlock(&p->lock);
  return flushed;
}

void ds_peer_close(ds_peer *p) {
  if (p == NULL) return;
  pthread_mutex_lock(&p->lock);
  p->stopping = 1;
  pthread_mutex_unlock(&p->lock);
  wake_io(p);
  pthread_join(p->thread, NULL);
  close(p->fd);
  close(p->wake[0]);
  close(p->wake[1]);
  clear_queue(&p->input);
  clear_queue(&p->output);
  ds_frame_clear(&p->incoming);
  pthread_cond_destroy(&p->changed);
  pthread_mutex_destroy(&p->lock);
  free(p);
}
