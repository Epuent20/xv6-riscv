#include "kernel/types.h"
#include "user/user.h"

int
main(void)
{
  int ticks = uptime();
  printf("up %d clock ticks\n", ticks);
  exit(0);
}
