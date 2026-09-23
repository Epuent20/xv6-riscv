#ifndef XV6_PSTAT_H
#define XV6_PSTAT_H

// CPU usage returned when a parent collects an exited child.
struct rusage {
  uint cputime;
};

#endif
