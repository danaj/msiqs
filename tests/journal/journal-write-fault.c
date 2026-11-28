/* Run the normal CLI with a real short journal write at a chosen frame. */
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include "../../msiqs.c"

static size_t fault_fwrite(const void *data, size_t size, size_t count, FILE *stream);
static int fault_fflush(FILE *stream);
static int fault_fstatvfs(int fd, struct statvfs *stats);
#define fwrite fault_fwrite
#define fflush fault_fflush
#define fstatvfs fault_fstatvfs
#include "../../siqs.c"
#undef fwrite
#undef fflush
#undef fstatvfs

static FILE *fault_stream;
static int report_enospc;

static int fault_fstatvfs(int fd, struct statvfs *stats) {
  int status = fstatvfs(fd, stats);
  if (status == 0 && getenv("SIQS_TEST_JOURNAL_LOW_SPACE") != NULL)
    stats->f_bavail = 0;
  return status;
}

static void arm_fault(const void *data, FILE *stream) {
  const char *mode = getenv("DMSIQS_TEST_JOURNAL_WRITE_FAULT");
  sj_reader header = {data, SJ_HEADER};
  uint32_t frame_type;
  size_t bytes;
  off_t position;
  uintmax_t target;
  struct rlimit limit;
  if (mode == NULL || sj_get32(&header) != SJ_MAGIC) return;
  frame_type = sj_get32(&header);
  if (frame_type == SJ_BATCH && strcmp(mode, "batch-header") == 0) {
    bytes = 11;
  } else if ((frame_type == SJ_BATCH && strcmp(mode, "batch-payload") == 0) ||
             (frame_type == SJ_ACCOUNTING && strcmp(mode, "accounting-payload") == 0)) {
    bytes = SJ_HEADER + 19;
    /* Exercise ENOSPC diagnostics with the same kernel-enforced short write. */
    report_enospc = 1;
  } else return;

  position = ftello(stream);
  if (position < 0 || (uintmax_t)position > UINTMAX_MAX - bytes ||
      getrlimit(RLIMIT_FSIZE, &limit) != 0)
    croak("journal write fault: cannot read journal position or file-size limit");
  target = (uintmax_t)position + bytes;
  if ((uintmax_t)(rlim_t)target != target || (rlim_t)target > limit.rlim_cur ||
      signal(SIGXFSZ, SIG_IGN) == SIG_ERR)
    croak("journal write fault: cannot configure file-size limit");
  limit.rlim_cur = (rlim_t)target;
  if (setrlimit(RLIMIT_FSIZE, &limit) != 0)
    croak("journal write fault: cannot set file-size limit");
  fault_stream = stream;
}

static size_t fault_fwrite(const void *data, size_t size, size_t count, FILE *stream) {
  size_t written;
  if (fault_stream == NULL && size == 1 && count == SJ_HEADER)
    arm_fault(data, stream);
  written = fwrite(data, size, count, stream);
  if (report_enospc && stream == fault_stream && (written != count || ferror(stream)) && errno == EFBIG)
    errno = ENOSPC;
  return written;
}

static int fault_fflush(FILE *stream) {
  int status = fflush(stream);
  if (report_enospc && stream == fault_stream && (status != 0 || ferror(stream)) && errno == EFBIG)
    errno = ENOSPC;
  return status;
}
