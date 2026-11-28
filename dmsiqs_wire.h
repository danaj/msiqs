#ifndef DMSIQS_WIRE_H
#define DMSIQS_WIRE_H

#include <stddef.h>
#include <stdint.h>

#define DS_VERSION 1U
#define DS_FRAME_MAX (1024U * 1024U)
#define DS_SETUP_MAX (16U * DS_FRAME_MAX)
#define DS_QUEUE_MAX (2U * DS_SETUP_MAX)
#define DS_REG_MAX_ENDPOINT 1024U
#define DS_REG_MAX_WORKERS 1024U
#define DS_CLAIM_CPU_CHECKPOINTS 1U
#define DS_CLAIM_PEAK_RSS 2U
#define DS_CPU_USAGE_BYTES 16U
#define DS_ENDED_USAGE_BYTES 24U
/* Exact DS_ERROR payload that permits retrying setup on a new connection. */
#define DS_SETUP_RETRY_ERROR "temporarily unable to start sieve pool"

enum ds_type {
  DS_CLAIM = 1, DS_GRANTED, DS_BUSY, DS_SETUP, DS_READY, DS_FAMILY,
  DS_DONE, DS_RELATIONS, DS_DIVISOR, DS_PAUSE, DS_PAUSED, DS_END,
  DS_ENDED, DS_ERROR, DS_PING, DS_PONG,
  DS_REG_REGISTER, DS_REG_REGISTERED, DS_REG_LIST, DS_REG_LISTED,
  DS_RELATIONS_CPU, DS_REG_REPORT_STALL, DS_REG_REGISTER_MANAGED,
  DS_REG_STALLED, DS_REG_REPAIRED
};
/* DS_RELATIONS_CPU appends two cumulative u64 process CPU deltas in
 * microseconds to DS_RELATIONS. Nonempty DS_ENDED carries the final pair,
 * followed by u64 peak process RSS in bytes if the claim requested it. */
typedef struct { uint64_t user_us, system_us; } ds_cpu_usage;

typedef struct { unsigned char *data; size_t size, alloc; int bad; } ds_buf;
typedef struct { const unsigned char *data; size_t left; int bad; } ds_reader;
typedef struct { unsigned type; unsigned char *data; size_t size; } ds_frame;
typedef struct ds_peer ds_peer;

void ds_put_u8(ds_buf *, uint8_t);
void ds_put_u32(ds_buf *, uint32_t);
void ds_put_u64(ds_buf *, uint64_t);
void ds_put_bytes(ds_buf *, const void *, size_t);
void ds_buf_clear(ds_buf *);
uint8_t ds_get_u8(ds_reader *);
uint32_t ds_get_u32(ds_reader *);
uint64_t ds_get_u64(ds_reader *);
const unsigned char *ds_get_bytes(ds_reader *, size_t);
int ds_reader_done(const ds_reader *);

/* FDs use -1 for errors. ds_peer_open takes ownership of the FD. */
int ds_listen(const char *address, const char *port);
int ds_connect(const char *endpoint, unsigned timeout_ms);
double ds_now(void);
/* Either output may be NULL. Peak RSS is in bytes, or zero when unavailable. */
int ds_process_usage(ds_cpu_usage *, uint64_t *peak_rss_bytes);
int ds_process_cpu(ds_cpu_usage *);
ds_peer *ds_peer_open(int fd);
/* Copies the payload. Fails on invalid arguments, full queue or lost peer. */
int ds_peer_send(ds_peer *, unsigned type, const ds_buf *);
/* Receive 1 frame, 0 if none, -1 after disconnection and input drain. */
int ds_peer_recv(ds_peer *, ds_frame *, unsigned timeout_ms);
/* Largest receive queue size in bytes, including each complete frame's node. */
size_t ds_peer_input_high_water(ds_peer *);
void ds_frame_clear(ds_frame *);
int ds_peer_alive(ds_peer *);
/* Give queued output a chance to reach the socket before closing. */
int ds_peer_flush(ds_peer *, unsigned timeout_ms);
void ds_peer_close(ds_peer *);

#endif
