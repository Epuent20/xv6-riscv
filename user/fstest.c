// fstest: tests for O_APPEND and rename.
// Note: xv6 does not count a directory's "." in its link count, so an
// empty directory has nlink 1 and each subdirectory adds 1 (its "..").
// usage: fstest            run all tests
//        fstest <name>     run tests whose name starts with <name>
//                          (e.g. "fstest append", "fstest rename")

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/fs.h"
#include "user/user.h"

static int failures;
static char *curtest;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      printf("  FAIL %s: line %d: %s\n", curtest, __LINE__, #cond);    \
      failures++;                                                      \
      return;                                                          \
    }                                                                  \
  } while (0)

static int
hasprefix(char *s, char *prefix)
{
  while (*prefix)
    if (*s++ != *prefix++)
      return 0;
  return 1;
}

static int
mkfile(char *path, char *data)
{
  int fd = open(path, O_CREATE | O_TRUNC | O_RDWR);
  if (fd < 0)
    return -1;
  int n = strlen(data);
  if (write(fd, data, n) != n) {
    close(fd);
    return -1;
  }
  close(fd);
  return 0;
}

static int
readall(char *path, char *buf, int max)
{
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  int n = 0, r;
  while (n < max && (r = read(fd, buf + n, max - n)) > 0)
    n += r;
  close(fd);
  if (n < max)
    buf[n] = 0;
  return n;
}

static uint
inum(char *path)
{
  struct stat st;
  if (stat(path, &st) < 0)
    return 0;
  return st.ino;
}

static int
nlink(char *path)
{
  struct stat st;
  if (stat(path, &st) < 0)
    return -1;
  return st.nlink;
}

// ---------------------------------------------------------------- O_APPEND

static void
append_basic(void)
{
  char buf[32];
  CHECK(mkfile("fst_l", "abc") == 0);
  int fd = open("fst_l", O_WRONLY | O_APPEND);
  CHECK(fd >= 0);
  CHECK(write(fd, "def", 3) == 3);
  close(fd);
  CHECK(readall("fst_l", buf, sizeof(buf)) == 6);
  CHECK(strcmp(buf, "abcdef") == 0);
  // without O_APPEND, a new open writes at offset 0
  fd = open("fst_l", O_WRONLY);
  CHECK(fd >= 0);
  CHECK(write(fd, "X", 1) == 1);
  close(fd);
  CHECK(readall("fst_l", buf, sizeof(buf)) == 6);
  CHECK(strcmp(buf, "Xbcdef") == 0);
  // O_APPEND with O_CREATE on a new file
  unlink("fst_l");
  fd = open("fst_l", O_CREATE | O_WRONLY | O_APPEND);
  CHECK(fd >= 0);
  CHECK(write(fd, "x", 1) == 1);
  CHECK(write(fd, "y", 1) == 1);
  close(fd);
  CHECK(readall("fst_l", buf, sizeof(buf)) == 2 && strcmp(buf, "xy") == 0);
  // O_APPEND with O_TRUNC: empty first, then append
  fd = open("fst_l", O_WRONLY | O_TRUNC | O_APPEND);
  CHECK(fd >= 0);
  CHECK(write(fd, "z", 1) == 1);
  close(fd);
  CHECK(readall("fst_l", buf, sizeof(buf)) == 1 && strcmp(buf, "z") == 0);
  unlink("fst_l");
}

static void
append_other_writer(void)
{
  // The end of file is found at EACH write, not at open: another
  // descriptor extends the file between two appends.
  char buf[32];
  CHECK(mkfile("fst_l", "abc") == 0);
  int a = open("fst_l", O_WRONLY | O_APPEND);
  int b = open("fst_l", O_WRONLY); // ordinary writer, offset 0
  CHECK(a >= 0 && b >= 0);
  CHECK(write(b, "1234567", 7) == 7); // file is now 7 bytes
  CHECK(write(a, "g", 1) == 1);       // must land at offset 7
  CHECK(write(b, "8", 1) == 1);       // b's offset is 7: overwrites 'g'
  CHECK(write(a, "h", 1) == 1);       // at offset 8
  close(a);
  close(b);
  CHECK(readall("fst_l", buf, sizeof(buf)) == 9);
  CHECK(strcmp(buf, "12345678h") == 0);
  unlink("fst_l");
}

static void
append_shared(void)
{
  // A dup'ed or inherited O_APPEND descriptor still appends, and a
  // read-only descriptor is unaffected by O_APPEND on another one.
  char buf[32];
  CHECK(mkfile("fst_l", "") == 0);
  int fd = open("fst_l", O_WRONLY | O_APPEND);
  CHECK(fd >= 0);
  int d = dup(fd);
  CHECK(write(fd, "A", 1) == 1 && write(d, "B", 1) == 1);
  if (fork() == 0) {
    write(d, "C", 1);
    exit(0);
  }
  wait(0);
  CHECK(write(fd, "D", 1) == 1);
  close(fd);
  close(d);
  CHECK(readall("fst_l", buf, sizeof(buf)) == 4 && strcmp(buf, "ABCD") == 0);
  unlink("fst_l");
}

#define NREC 200
#define RECSZ 16

static void
append_concurrent(void)
{
  // Two processes, each with its OWN open of the file, append
  // records. With O_APPEND no record may be lost or torn.
  static char buf[2 * NREC * RECSZ + 1];
  struct stat st;
  int fd = open("fst_l", O_CREATE | O_TRUNC | O_WRONLY);
  CHECK(fd >= 0);
  close(fd);
  for (int c = 0; c < 2; c++) {
    if (fork() == 0) {
      int f = open("fst_l", O_WRONLY | O_APPEND);
      char rec[RECSZ];
      for (int i = 0; i < NREC; i++) {
        memset(rec, 'a' + c, RECSZ);
        rec[RECSZ - 1] = '\n';
        if (write(f, rec, RECSZ) != RECSZ)
          exit(1);
      }
      exit(0);
    }
  }
  int s1, s2;
  wait(&s1);
  wait(&s2);
  CHECK(s1 == 0 && s2 == 0);
  CHECK(stat("fst_l", &st) == 0 && st.size == 2 * NREC * RECSZ);
  CHECK(readall("fst_l", buf, sizeof(buf)) == 2 * NREC * RECSZ);
  int count[2] = {0, 0};
  for (int r = 0; r < 2 * NREC; r++) {
    char *p = buf + r * RECSZ;
    CHECK(p[0] == 'a' || p[0] == 'b');
    for (int j = 1; j < RECSZ - 1; j++)
      CHECK(p[j] == p[0]); // not torn
    CHECK(p[RECSZ - 1] == '\n');
    count[p[0] - 'a']++;
  }
  CHECK(count[0] == NREC && count[1] == NREC);
  unlink("fst_l");
}

// ---------------------------------------------------------------- rename

static void
rename_file(void)
{
  char buf[16];
  CHECK(mkfile("fst_r1", "one") == 0);
  uint i1 = inum("fst_r1");
  CHECK(rename("fst_r1", "fst_r2") == 0);
  CHECK(open("fst_r1", O_RDONLY) < 0);
  CHECK(inum("fst_r2") == i1); // same inode, new name
  CHECK(readall("fst_r2", buf, sizeof(buf)) == 3 && strcmp(buf, "one") == 0);
  CHECK(nlink("fst_r2") == 1);
  CHECK(rename("fst_nosuch", "fst_r3") == -1);
  CHECK(rename("fst_r2", "fst_nodir/x") == -1);
  CHECK(nlink("fst_r2") == 1); // failed rename changed nothing
  unlink("fst_r2");
}

static void
rename_replace(void)
{
  char buf[16];
  CHECK(mkfile("fst_r1", "new") == 0);
  CHECK(mkfile("fst_r2", "old") == 0);
  CHECK(link("fst_r2", "fst_r2b") == 0); // a second name for the target
  CHECK(nlink("fst_r2b") == 2);
  uint i1 = inum("fst_r1");
  CHECK(rename("fst_r1", "fst_r2") == 0);
  CHECK(inum("fst_r2") == i1);
  CHECK(readall("fst_r2", buf, sizeof(buf)) == 3 && strcmp(buf, "new") == 0);
  CHECK(nlink("fst_r2b") == 1); // the replaced inode lost one link
  CHECK(readall("fst_r2b", buf, sizeof(buf)) == 3 && strcmp(buf, "old") == 0);
  // the replaced file stays readable through an open fd
  int fd = open("fst_r2b", O_RDONLY);
  CHECK(fd >= 0);
  CHECK(rename("fst_r2", "fst_r2b") == 0);
  CHECK(read(fd, buf, 3) == 3 && buf[0] == 'o');
  close(fd);
  CHECK(open("fst_r2", O_RDONLY) < 0);
  unlink("fst_r2b");
}

static void
rename_self(void)
{
  CHECK(mkfile("fst_r1", "x") == 0);
  CHECK(rename("fst_r1", "fst_r1") == 0);
  CHECK(link("fst_r1", "fst_r1b") == 0);
  CHECK(rename("fst_r1", "fst_r1b") == 0); // same inode: no-op
  CHECK(nlink("fst_r1") == 2 && nlink("fst_r1b") == 2);
  CHECK(rename("fst_r1", ".") == -1);
  CHECK(rename("fst_r1", "..") == -1);
  CHECK(rename(".", "fst_dot") == -1);
  unlink("fst_r1");
  unlink("fst_r1b");
}

static void
rename_crossdir(void)
{
  char buf[16];
  CHECK(mkdir("fst_d1") == 0 && mkdir("fst_d2") == 0);
  CHECK(mkfile("fst_d1/f", "data") == 0);
  CHECK(rename("fst_d1/f", "fst_d2/g") == 0);
  CHECK(open("fst_d1/f", O_RDONLY) < 0);
  CHECK(readall("fst_d2/g", buf, sizeof(buf)) == 4);
  CHECK(nlink("fst_d1") == 1 && nlink("fst_d2") == 1); // files don't change dir links
  CHECK(unlink("fst_d1") == 0); // fst_d1 is empty again
  CHECK(unlink("fst_d2/g") == 0 && unlink("fst_d2") == 0);
}

static void
rename_dir(void)
{
  char buf[16];
  CHECK(mkdir("fst_p1") == 0 && mkdir("fst_p2") == 0);
  CHECK(mkdir("fst_p1/sub") == 0);
  CHECK(mkfile("fst_p1/sub/f", "inside") == 0);
  uint isub = inum("fst_p1/sub");
  CHECK(nlink("fst_p1") == 2 && nlink("fst_p2") == 1);
  CHECK(rename("fst_p1/sub", "fst_p2/sub2") == 0);
  CHECK(inum("fst_p2/sub2") == isub);
  CHECK(inum("fst_p2/sub2/..") == inum("fst_p2")); // ".." updated
  CHECK(nlink("fst_p1") == 1 && nlink("fst_p2") == 2);
  CHECK(nlink("fst_p2/sub2") == 1);
  CHECK(readall("fst_p2/sub2/f", buf, sizeof(buf)) == 6);
  // ".." really works: chdir in and walk back out
  uint home = inum(".");
  CHECK(chdir("fst_p2/sub2") == 0);
  CHECK(chdir("../..") == 0);
  CHECK(inum(".") == home);
  // same-directory rename of a directory
  CHECK(rename("fst_p2/sub2", "fst_p2/sub3") == 0);
  CHECK(inum("fst_p2/sub3/..") == inum("fst_p2") && nlink("fst_p2") == 2);
  CHECK(unlink("fst_p1") == 0);
  CHECK(unlink("fst_p2") == -1); // not empty
  CHECK(unlink("fst_p2/sub3/f") == 0 && unlink("fst_p2/sub3") == 0);
  CHECK(nlink("fst_p2") == 1 && unlink("fst_p2") == 0);
}

static void
rename_cycle(void)
{
  CHECK(mkdir("fst_c") == 0 && mkdir("fst_c/x") == 0 && mkdir("fst_c/x/y") == 0);
  CHECK(rename("fst_c", "fst_c/x/y/z") == -1); // into own subtree
  CHECK(rename("fst_c", "fst_c/z") == -1);
  CHECK(rename("fst_c/x", "fst_c/x/y/x") == -1);
  CHECK(nlink("fst_c") == 2 && nlink("fst_c/x") == 2);
  CHECK(inum("fst_c/x/y/..") == inum("fst_c/x"));
  // moving a directory UP is fine
  CHECK(rename("fst_c/x/y", "fst_c/y") == 0);
  CHECK(inum("fst_c/y/..") == inum("fst_c"));
  CHECK(nlink("fst_c") == 3 && nlink("fst_c/x") == 1);
  // a file onto its own ancestor directory: must fail, not deadlock
  CHECK(mkfile("fst_c/x/f", "f") == 0);
  CHECK(rename("fst_c/x/f", "fst_c/x") == -1);
  CHECK(rename("fst_c/x/f", "fst_c") == -1);
  CHECK(unlink("fst_c/x/f") == 0);
  CHECK(unlink("fst_c/y") == 0 && unlink("fst_c/x") == 0 && unlink("fst_c") == 0);
}

static void
rename_types(void)
{
  CHECK(mkdir("fst_t1") == 0 && mkdir("fst_t2") == 0 && mkdir("fst_t3") == 0);
  CHECK(mkfile("fst_t3/f", "f") == 0);
  CHECK(mkfile("fst_tf", "f") == 0);
  CHECK(rename("fst_tf", "fst_t1") == -1); // file onto dir
  CHECK(rename("fst_t1", "fst_tf") == -1); // dir onto file
  CHECK(rename("fst_t1", "fst_t3") == -1); // dir onto non-empty dir
  uint i1 = inum("fst_t1");
  CHECK(mkdir("fst_t1/in") == 0);
  CHECK(rename("fst_t1", "fst_t2") == 0); // dir onto empty dir
  CHECK(inum("fst_t2") == i1 && inum("fst_t2/in/..") == i1);
  CHECK(nlink("fst_t2") == 2);
  CHECK(open("fst_t1", O_RDONLY) < 0);
  CHECK(unlink("fst_t2/in") == 0 && unlink("fst_t2") == 0);
  CHECK(unlink("fst_t3/f") == 0 && unlink("fst_t3") == 0 && unlink("fst_tf") == 0);
}

static void
rename_concurrent(void)
{
  // Renames in opposite directions between two directories, plus
  // create/unlink traffic. Must not deadlock or corrupt link counts.
  CHECK(mkdir("fst_x") == 0 && mkdir("fst_y") == 0);
  CHECK(mkdir("fst_x/a") == 0 && mkdir("fst_y/b") == 0);
  int pids[3];
  for (int c = 0; c < 3; c++) {
    if ((pids[c] = fork()) == 0) {
      for (int i = 0; i < 100; i++) {
        if (c == 0) {
          if (rename("fst_x/a", "fst_y/a") != 0 || rename("fst_y/a", "fst_x/a") != 0)
            exit(1);
        } else if (c == 1) {
          if (rename("fst_y/b", "fst_x/b") != 0 || rename("fst_x/b", "fst_y/b") != 0)
            exit(1);
        } else {
          // try to move x into y and y into x: these race with the
          // others and may fail, but must never create a cycle
          rename("fst_x", "fst_y/b/xx");
          rename("fst_y/b/xx", "fst_x");
          mkfile("fst_x/t", "t");
          unlink("fst_x/t");
        }
      }
      exit(0);
    }
  }
  int st, bad = 0;
  for (int c = 0; c < 3; c++) {
    wait(&st);
    if (st != 0)
      bad = 1;
  }
  if (inum("fst_x") == 0) // child 2 may have left fst_x inside fst_y/b
    rename("fst_y/b/xx", "fst_x");
  CHECK(!bad);
  CHECK(inum("fst_x/a/..") == inum("fst_x"));
  CHECK(inum("fst_y/b/..") == inum("fst_y"));
  CHECK(nlink("fst_x") == 2 && nlink("fst_y") == 2);
  CHECK(unlink("fst_x/a") == 0 && unlink("fst_y/b") == 0);
  CHECK(unlink("fst_x") == 0 && unlink("fst_y") == 0);
}

static void
rename_cycle_race(void)
{
  // Two processes try to move two directories into each other.
  // Each rename alone is legal; doing both would detach a cycle
  // from the tree. At most one may succeed at any moment.
  CHECK(mkdir("fst_m") == 0 && mkdir("fst_n") == 0);
  for (int c = 0; c < 2; c++) {
    if (fork() == 0) {
      for (int i = 0; i < 1000; i++) {
        if (c == 0) {
          if (rename("fst_m", "fst_n/m") == 0)
            rename("fst_n/m", "fst_m");
        } else {
          if (rename("fst_n", "fst_m/n") == 0)
            rename("fst_m/n", "fst_n");
        }
      }
      exit(0);
    }
  }
  wait(0);
  wait(0);
  // both must be back at the top level and still reachable
  CHECK(inum("fst_m") != 0 && inum("fst_n") != 0);
  CHECK(inum("fst_m/..") == inum(".") && inum("fst_n/..") == inum("."));
  CHECK(unlink("fst_m") == 0 && unlink("fst_n") == 0);
}

struct test {
  void (*f)(void);
  char *name;
} tests[] = {
  {append_basic, "append_basic"},
  {append_other_writer, "append_other_writer"},
  {append_shared, "append_shared"},
  {append_concurrent, "append_concurrent"},
  {rename_file, "rename_file"},
  {rename_replace, "rename_replace"},
  {rename_self, "rename_self"},
  {rename_crossdir, "rename_crossdir"},
  {rename_dir, "rename_dir"},
  {rename_cycle, "rename_cycle"},
  {rename_types, "rename_types"},
  {rename_concurrent, "rename_concurrent"},
  {rename_cycle_race, "rename_cycle_race"},
  {0, 0},
};

int
main(int argc, char *argv[])
{
  char *prefix = argc > 1 ? argv[1] : "";
  int ran = 0;
  for (struct test *t = tests; t->name; t++) {
    if (!hasprefix(t->name, prefix))
      continue;
    int before = failures;
    curtest = t->name;
    printf("test %s: ", t->name);
    t->f();
    if (failures == before)
      printf("OK\n");
    ran++;
  }
  if (ran == 0)
    printf("fstest: no test matches %s\n", prefix);
  else if (failures == 0)
    printf("fstest: ALL %d TESTS PASSED\n", ran);
  else
    printf("fstest: %d FAILURE(S)\n", failures);
  exit(failures != 0);
}
