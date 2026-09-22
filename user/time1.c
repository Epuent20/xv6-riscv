#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  if(argc < 2){
    fprintf(2, "usage: time1 command [args...]\n");
    exit(1);
  }

  int start = uptime();
  int pid = fork();

  if(pid < 0){
    fprintf(2, "time1: fork failed\n");
    exit(1);
  }

  if(pid == 0){
    // Child: replace this program with the requested command.
    exec(argv[1], &argv[1]);

    // Successful exec never returns.
    fprintf(2, "time1: exec %s failed\n", argv[1]);
    exit(1);
  }

  // Parent: wait for the command to finish.
  int status;
  if(wait(&status) < 0){
    fprintf(2, "time1: wait failed\n");
    exit(1);
  }

  int elapsed = uptime() - start;
  printf("elapsed time: %d ticks\n", elapsed);

  exit(status);
}
