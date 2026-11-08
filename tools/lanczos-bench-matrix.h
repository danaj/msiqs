/* LCMAT001 reduced matrices: little-endian uint32 dimensions and columns.
 * Input loading and the FNV-1a provenance hash are outside solver timings. */
#include <sys/stat.h>

static void bench_matrix_read(FILE *file, unsigned char *bytes, size_t count,
                               uint64_t *hash) {
  size_t i;
  if (fread(bytes, 1, count, file) != count)
    croak("bench-lanczos: truncated matrix file");
  for (i = 0; i < count; i++) {
    *hash ^= bytes[i];
    *hash *= UINT64_C(1099511628211);
  }
}

static uint32_t bench_matrix_u32(FILE *file, uint64_t *hash) {
  unsigned char b[4];
  bench_matrix_read(file, b, sizeof(b), hash);
  return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
         ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static la_col_t *bench_load_matrix(const char *path, uint32_t *rows,
                                    uint32_t *count, size_t *entries,
                                    uint64_t *hash) {
  FILE *file = fopen(path, "rb");
  struct stat status;
  unsigned char magic[8];
  uint64_t remaining;
  uint32_t c, i;
  la_col_t *cols;
  if (file == NULL)
    croak("bench-lanczos: cannot open matrix %s: %s", path, strerror(errno));
  if (fstat(fileno(file), &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_size < 20)
    croak("bench-lanczos: matrix must be a regular file with a complete header");
  *hash = UINT64_C(14695981039346656037);
  bench_matrix_read(file, magic, sizeof(magic), hash);
  if (memcmp(magic, "LCMAT001", sizeof(magic)) != 0)
    croak("bench-lanczos: invalid matrix magic");
  *rows = bench_matrix_u32(file, hash);
  *count = bench_matrix_u32(file, hash);
  if (bench_matrix_u32(file, hash) != 0)
    croak("bench-lanczos: matrix dense_rows must be zero");
  if (*rows == 0 || *count == 0)
    croak("bench-lanczos: matrix dimensions must be nonzero");
  remaining = (uint64_t)status.st_size - 20U;
  if ((uint64_t)*count > remaining / 8U)
    croak("bench-lanczos: truncated matrix column headers");
  cols = (la_col_t *)nla_calloc(*count, sizeof(*cols));
  *entries = 0;
  for (c = 0; c < *count; c++) {
    if (remaining < 8U) croak("bench-lanczos: truncated matrix column header");
    cols[c].weight = bench_matrix_u32(file, hash);
    cols[c].orig = bench_matrix_u32(file, hash);
    remaining -= 8U;
    if ((uint64_t)cols[c].weight > remaining / 4U)
      croak("bench-lanczos: truncated matrix column payload");
    if ((size_t)cols[c].weight > (size_t)-1 - *entries)
      croak("bench-lanczos: matrix weight overflow");
    *entries += cols[c].weight;
    cols[c].data = (uint32_t *)nla_malloc(cols[c].weight, sizeof(*cols[c].data));
    for (i = 0; i < cols[c].weight; i++) {
      cols[c].data[i] = bench_matrix_u32(file, hash);
      if (cols[c].data[i] >= *rows)
        croak("bench-lanczos: matrix row is out of range");
    }
    remaining -= (uint64_t)cols[c].weight * 4U;
  }
  if (remaining != 0 || fgetc(file) != EOF || ferror(file))
    croak("bench-lanczos: trailing matrix data or read error");
  if (fclose(file) != 0) croak("bench-lanczos: matrix close failed");
  return cols;
}
