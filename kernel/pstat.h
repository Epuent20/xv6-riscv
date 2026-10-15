#ifndef XV6_PSTAT_H
#define XV6_PSTAT_H

// Shared process states used by the kernel and ps.
enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// CPU usage returned when a parent collects an exited child.
struct rusage {
  uint cputime;
};

// One process record returned by getprocs().
struct pstat {
  int pid;
  enum procstate state;
  uint64 size;
  int ppid;
  char name[16];
};

#endif
