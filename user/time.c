#include "kernel/types.h"
#include "kernel/pstat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  if(argc < 2){
    fprintf(2, "usage: time command [args...]\n");
    exit(1);
  }

  // Start measuring before creating the child.
  int start = uptime();
  int pid = fork();

  if(pid < 0){
    fprintf(2, "time: fork failed\n");
    exit(1);
  }

  if(pid == 0){
    // Pass the command and its arguments to the new program.
    exec(argv[1], &argv[1]);

    fprintf(2, "time: exec %s failed\n", argv[1]);
    exit(1);
  }

  int status;
  struct rusage usage = {0};

  // Wait for the child and retrieve its CPU usage.
  if(wait2(&status, &usage) < 0){
    fprintf(2, "time: wait2 failed\n");
    exit(1);
  }

  int elapsed = uptime() - start;

  // Use wider arithmetic to avoid overflow during multiplication.
  if(elapsed > 0){
    int percent = (int)(((uint64)usage.cputime * 100) / elapsed);

    printf("elapsed time: %d ticks, cpu time: %d ticks, %d%% CPU\n",
           elapsed, (int)usage.cputime, percent);
  } else {
    // Whole-tick timing cannot determine a percentage here.
    printf("elapsed time: 0 ticks, cpu time: %d ticks, N/A%% CPU\n",
           (int)usage.cputime);
  }

  exit(status);
}
