#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

#define LARGE 8192
#define SMALL 16
#define LARGE_COUNT 8
#define SMALL_COUNT 256

struct writer {
  char tag;
  int size;
  int count;
  int append;
};

static uchar record[LARGE];

static int
read_full(int fd, void *dst, int n)
{
  char *p = dst;
  while (n > 0) {
    int got = read(fd, p, n);
    if (got <= 0)
      return -1;
    p += got;
    n -= got;
  }
  return 0;
}

static void
encode_record(struct writer w, int index)
{
  memset(record, w.tag, w.size);
  record[1] = index & 255;
  record[2] = index >> 8;
}

static int
valid_record(struct writer w, int index)
{
  if (record[0] != w.tag || record[1] != (index & 255) ||
      record[2] != (index >> 8))
    return 0;
  for (int i = 3; i < w.size; i++)
    if (record[i] != w.tag)
      return 0;
  return 1;
}

static void
worker(const char *path, struct writer w, int ready[2], int start[2])
{
  close(ready[0]);
  close(start[1]);

  int fd = open(path, O_WRONLY | (w.append ? O_APPEND : 0));
  char token = 'R';
  if (fd < 0 || write(ready[1], &token, 1) != 1)
    exit(1);
  close(ready[1]);

  if (read(start[0], &token, 1) != 1)
    exit(1);
  close(start[0]);

  for (int i = 0; i < w.count; i++) {
    encode_record(w, i);
    // One write syscall per logical record.
    if (write(fd, record, w.size) != w.size)
      exit(1);
  }
  close(fd);
  exit(0);
}

static int
run_writers(const char *path, const struct writer w[2])
{
  int ready[2], start[2];
  char tokens[2];

  if (pipe(ready) < 0)
    return -1;
  if (pipe(start) < 0) {
    close(ready[0]);
    close(ready[1]);
    return -1;
  }

  int started = 0;
  for (; started < 2; started++) {
    int pid = fork();
    if (pid < 0)
      break;
    if (pid == 0) {
      worker(path, w[started], ready, start);
      exit(1);
    }
  }
  close(ready[1]);
  close(start[0]);

  // Both writers have independently opened the file before release.
  int ok = started == 2 && read_full(ready[0], tokens, 2) == 0 &&
           write(start[1], tokens, 2) == 2;
  close(ready[0]);
  close(start[1]);

  for (int i = 0; i < started; i++) {
    int status;
    if (wait(&status) < 0 || status != 0)
      ok = 0;
  }
  return ok ? 0 : -1;
}

static int
check_file(int fd, const struct writer w[2], int prefix_bytes, int expected)
{
  struct stat st;
  if (fstat(fd, &st) < 0)
    return -1;
  if (st.size != (uint64)expected) {
    fprintf(2, "  wrong file length: expected %d, got %d bytes\n",
            expected, (int)st.size);
    return -1;
  }

  int seen[2] = {0, 0};
  int off = 0;
  // A normal writer owns this prefix so it never overwrites an append.
  while (off < prefix_bytes) {
    if (read_full(fd, record, w[1].size) < 0 ||
        !valid_record(w[1], seen[1])) {
      fprintf(2, "  bad ordinary-write record at byte %d\n", off);
      return -1;
    }
    seen[1]++;
    off += w[1].size;
  }

  while (off < expected) {
    if (read_full(fd, record, 1) < 0)
      return -1;
    int slot = record[0] == w[0].tag ? 0 : 1;
    struct writer cur = w[slot];
    if (record[0] != cur.tag || !cur.append || seen[slot] >= cur.count ||
        cur.size > expected - off ||
        read_full(fd, record + 1, cur.size - 1) < 0 ||
        !valid_record(cur, seen[slot])) {
      fprintf(2, "  record broken or out of order at byte %d\n", off);
      return -1;
    }
    seen[slot]++;
    off += cur.size;
  }

  if (seen[0] != w[0].count || seen[1] != w[1].count) {
    fprintf(2, "  wrong record counts: got %d and %d\n", seen[0], seen[1]);
    return -1;
  }
  return 0;
}

static int
run_case(const char *name, const char *path, const struct writer w[2])
{
  int expected = w[0].size * w[0].count + w[1].size * w[1].count;
  int prefix_bytes = w[1].append ? 0 : w[1].size * w[1].count;

  printf("appendtest: %s\n", name);
  int fd = open(path, O_CREATE | O_TRUNC | O_WRONLY);
  if (fd < 0) {
    fprintf(2, "  could not create scratch file %s\n", path);
    return -1;
  }
  if (prefix_bytes > 0) {
    memset(record, 0, prefix_bytes);
    if (write(fd, record, prefix_bytes) != prefix_bytes) {
      close(fd);
      unlink(path);
      fprintf(2, "  could not reserve the ordinary-write prefix\n");
      return -1;
    }
  }
  close(fd);

  int ok = run_writers(path, w) == 0;
  if (!ok) {
    fprintf(2, "  writer or pipe failed\n");
  } else {
    fd = open(path, O_RDONLY);
    if (fd < 0) {
      fprintf(2, "  could not reopen scratch file %s\n", path);
      ok = 0;
    } else {
      ok = check_file(fd, w, prefix_bytes, expected) == 0;
      close(fd);
    }
  }
  if (unlink(path) < 0) {
    fprintf(2, "  could not remove scratch file %s\n", path);
    ok = 0;
  }
  if (ok)
    printf("  PASS: %d bytes, %d records from A, %d from B\n",
           expected, w[0].count, w[1].count);
  else
    printf("  FAIL: %s\n", name);
  return ok ? 0 : -1;
}

int
main(void)
{
  const struct writer two_large[2] = {
    {'A', LARGE, LARGE_COUNT, 1}, {'B', LARGE, LARGE_COUNT, 1},
  };
  const struct writer mixed[2] = {
    {'A', LARGE, LARGE_COUNT, 1}, {'B', SMALL, SMALL_COUNT, 1},
  };
  const struct writer ordinary[2] = {
    {'A', LARGE, LARGE_COUNT, 1}, {'B', SMALL, SMALL_COUNT, 0},
  };

  int failed = 0;
  failed += run_case("large + large append", "pa5-big.tmp", two_large) < 0;
  failed += run_case("large + small append", "pa5-mix.tmp", mixed) < 0;
  failed += run_case("large append + ordinary writer", "pa5-ord.tmp", ordinary) < 0;

  if (failed == 0)
    printf("appendtest: ALL 3 TESTS PASSED\n");
  else
    printf("appendtest: %d of 3 cases failed\n", failed);
  exit(failed != 0);
}
