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

  // Prevent the compiler from optimizing the loop away.
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
   * Different workloads:
   *
   * 0,1 : CPU-heavy
   * 2,3 : CPU bursts separated by voluntary sleep
   * 4   : shorter CPU workload
   * 5   : long CPU workload
   */
  if(id == 0){
    cpu_burst(12000000);
    cpu_burst(12000000);
  }
  else if(id == 1){
    cpu_burst(8000000);
    cpu_burst(8000000);
    cpu_burst(8000000);
  }
  else if(id == 2){
    cpu_burst(3000000);
    pause(5);
    cpu_burst(3000000);
    pause(5);
    cpu_burst(3000000);
  }
  else if(id == 3){
    cpu_burst(2000000);
    pause(8);
    cpu_burst(5000000);
    pause(8);
    cpu_burst(2000000);
  }
  else if(id == 4){
    cpu_burst(5000000);
  }
  else {
    cpu_burst(25000000);
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
