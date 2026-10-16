#include "kernel/types.h"
#include "user/user.h"

#define NCHILD 2
#define WORK_ITERATIONS 200000000

struct result {
  int pid;
  int priority;
  int first_tick;
  int finish_tick;
};

// A pipe read can return fewer bytes than requested.
static int
read_full(int fd, void *buffer, int size)
{
  char *p = buffer;
  int done = 0;
  int n;

  while (done < size) {
    n = read(fd, p + done, size - done);
    if (n <= 0)
      return -1;
    done += n;
  }
  return 0;
}

static void
do_work(void)
{
  volatile uint64 sum = 0;

  for (int i = 0; i < WORK_ITERATIONS; i++)
    sum += i;
}

int
main(int argc, char *argv[])
{
  int ready[2];
  int report[2];
  int start[NCHILD][2];
  int priorities[NCHILD] = {0, 20};
  struct result results[NCHILD];
  char token = 'R';
  int release_tick;

  (void)argc;
  (void)argv;

  if (pipe(ready) < 0 || pipe(report) < 0 ||
      pipe(start[0]) < 0 || pipe(start[1]) < 0) {
    fprintf(2, "schedtest: pipe failed\n");
    exit(1);
  }

  // Keep the parent runnable while it prepares both children.
  if (setpriority(49) < 0) {
    fprintf(2, "schedtest: parent priority failed\n");
    exit(1);
  }

  for (int i = 0; i < NCHILD; i++) {
    int pid = fork();

    if (pid < 0) {
      fprintf(2, "schedtest: fork failed\n");
      exit(1);
    }

    if (pid == 0) {
      close(ready[0]);
      close(report[0]);

      for (int j = 0; j < NCHILD; j++) {
        close(start[j][1]);
        if (j != i)
          close(start[j][0]);
      }

      if (setpriority(priorities[i]) < 0 ||
          write(ready[1], &token, 1) != 1) {
        fprintf(2, "schedtest: child setup failed\n");
        exit(1);
      }
      close(ready[1]);

      // Wait until the parent has prepared both children.
      if (read(start[i][0], &token, 1) != 1) {
        fprintf(2, "schedtest: start signal failed\n");
        exit(1);
      }
      close(start[i][0]);

      struct result r;
      r.pid = getpid();
      r.priority = priorities[i];
      r.first_tick = uptime();
      do_work();
      r.finish_tick = uptime();

      if (write(report[1], &r, (int)sizeof(r)) != (int)sizeof(r)) {
        fprintf(2, "schedtest: result write failed\n");
        exit(1);
      }
      close(report[1]);
      exit(0);
    }
  }

  close(ready[1]);
  close(report[1]);
  for (int i = 0; i < NCHILD; i++)
    close(start[i][0]);

  // Both children have set their priorities before either starts work.
  for (int i = 0; i < NCHILD; i++) {
    if (read(ready[0], &token, 1) != 1) {
      fprintf(2, "schedtest: child did not become ready\n");
      exit(1);
    }
  }
  close(ready[0]);

  release_tick = uptime();
  for (int i = 0; i < NCHILD; i++) {
    if (write(start[i][1], &token, 1) != 1) {
      fprintf(2, "schedtest: could not release child\n");
      exit(1);
    }
    close(start[i][1]);
  }

  // Results arrive in completion order. Read both before printing.
  for (int i = 0; i < NCHILD; i++) {
    if (read_full(report[0], &results[i], (int)sizeof(results[i])) < 0) {
      fprintf(2, "schedtest: missing result\n");
      exit(1);
    }
  }
  close(report[0]);

  for (int i = 0; i < NCHILD; i++) {
    int status;
    if (wait(&status) < 0 || status != 0) {
      fprintf(2, "schedtest: child failed\n");
      exit(1);
    }
  }

  printf("released at tick %d; results in completion order:\n",
         release_tick);
  for (int i = 0; i < NCHILD; i++) {
    struct result *r = &results[i];
    printf("pid %d priority %d: first %d, finish %d, response %d, turnaround %d\n",
           r->pid, r->priority, r->first_tick, r->finish_tick,
           r->first_tick - release_tick, r->finish_tick - release_tick);
  }

  exit(0);
}
