#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

#define NCHILD 6

static void
cpu_burst(int n)
{
  volatile unsigned long x = 0;

  for(int i = 0; i < n; i++){
    x += i * 31;
    x ^= (x << 7);
    x ^= (x >> 3);
  }

  if(x == 0xFFFFFFFFFFFFFFFFUL)
    printf("x=%lu\n", x);
}

static void
run_child(int id)
{
  int start = uptime();

  printf("schedulertest: child %d pid %d start %d\n",
         id, getpid(), start);

  /*
   * Child 0:
   * Long CPU-bound workload.
   * Designed to fall through multiple MLFQ queues.
   */
  if(id == 0){
    cpu_burst(30000000);
    cpu_burst(30000000);
    cpu_burst(30000000);
  }

  /*
   * Child 1:
   * Very long CPU-bound workload.
   */
  else if(id == 1){
    cpu_burst(40000000);
    cpu_burst(40000000);
    cpu_burst(40000000);
  }

  /*
   * Child 2:
   * CPU burst -> voluntary sleep -> CPU burst.
   * Sleeping must NOT demote the process.
   */
  else if(id == 2){
    cpu_burst(10000000);
    pause(5);

    cpu_burst(10000000);
    pause(5);

    cpu_burst(10000000);
  }

  /*
   * Child 3:
   * Multiple voluntary sleeps with CPU work between them.
   */
  else if(id == 3){
    cpu_burst(8000000);
    pause(8);

    cpu_burst(16000000);
    pause(8);

    cpu_burst(8000000);
  }

  /*
   * Child 4:
   * Medium CPU workload.
   */
  else if(id == 4){
    cpu_burst(15000000);
    cpu_burst(15000000);
  }

  /*
   * Child 5:
   * Long CPU workload to ensure the test crosses
   * the 48-tick global boost interval.
   */
  else {
    cpu_burst(60000000);
    cpu_burst(60000000);
  }

  printf("schedulertest: child %d pid %d finish %d\n",
         id, getpid(), uptime());

  exit(0);
}

int
main(void)
{
  int start = uptime();

  printf("schedulertest: start %d\n", start);

  for(int i = 0; i < NCHILD; i++){
    int pid = fork();

    if(pid < 0){
      printf("schedulertest: fork failed\n");
      exit(1);
    }

    if(pid == 0)
      run_child(i);
  }

  for(int i = 0; i < NCHILD; i++)
    wait(0);

  printf("schedulertest: all children finished at %d\n", uptime());

  exit(0);
}
