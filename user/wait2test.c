#include "kernel/types.h"
#include "kernel/pstat.h"
#include "user/user.h"

int
main(void)
{
  int pid = fork();

  if(pid < 0){
    fprintf(2, "wait2test: fork failed\n");
    exit(1);
  }

  if(pid == 0){
    // Stay active long enough to receive several timer interrupts.
    int start = uptime();
    while(uptime() - start < 10)
      ;

    // Use a recognizable status to check that wait2 returns it.
    exit(7);
  }

  int status = -1;
  struct rusage usage = {0};

  // Collect the child's PID, exit status, and CPU usage.
  int reaped = wait2(&status, &usage);

  if(reaped < 0){
    fprintf(2, "wait2test: wait2 failed\n");
    exit(1);
  }

  printf("expected pid=%d, returned pid=%d\n", pid, reaped);
  printf("status=%d, cpu ticks=%d\n", status, (int)usage.cputime);

  exit(0);
}
