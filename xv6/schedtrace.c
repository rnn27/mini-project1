#include "kernel/types.h"
#include "user/user.h"

int
main(void)
{
  int pid;

  schedtrace_reset();

  pid = fork();

  if(pid < 0){
    printf("schedtrace: fork failed\n");
    exit(1);
  }

  if(pid == 0){
    char *argv[] = {"schedulertest", 0};
    exec("schedulertest", argv);
    printf("schedtrace: exec failed\n");
    exit(1);
  }

  wait(0);
  schedtrace_dump();
  exit(0);
}
