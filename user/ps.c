#include "kernel/param.h"
#include "kernel/types.h"
#include "kernel/pstat.h"
#include "user/user.h"

int
main(int argc, char **argv)
{
  struct pstat uproc[NPROC];
  int nprocs;
  int i;
  uint now;
  char *state;
  static char *states[] = {
    [SLEEPING]  "sleeping",
    [RUNNABLE]  "runnable",
    [RUNNING]   "running ",
    [ZOMBIE]    "zombie  "
  };

  nprocs = getprocs(uproc);
  if (nprocs < 0)
    exit(-1);
  now = uptime();

  printf("pid\tstate\t\tsize\tppid\tpriority\tage\tname\n");
  for (i=0; i<nprocs; i++) {
    state = states[uproc[i].state];

    printf("%d\t%s\t%lu\t%d\t%d\t\t",
           uproc[i].pid, state, uproc[i].size, uproc[i].ppid,
           uproc[i].priority);

    // Only RUNNABLE processes are currently waiting for CPU time.
    if (uproc[i].state == RUNNABLE)
      printf("%d\t", (int)(now - uproc[i].readytime));
    else
      printf("-\t");

    printf("%s\n", uproc[i].name);
  }

  exit(0);
}
