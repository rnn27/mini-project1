#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if (pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int)(p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;

  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for (p = proc; p < &proc[NPROC]; p++) {
    initlock(&p->lock, "proc");
    p->state = UNUSED;
    p->kstack = KSTACK((int)(p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu *
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc *
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

#ifdef SCHEDULER_MLFQ
#define MLFQ_QUEUES 4
#define MLFQ_BOOST_INTERVAL 48

#define MLFQ_TRACE_SIZE 4096

struct mlfq_trace_entry {
  uint64 tick;
  int pid;
  int queue;
};

static struct mlfq_trace_entry mlfq_trace[MLFQ_TRACE_SIZE];
static uint64 mlfq_trace_count = 0;

void
mlfq_trace_reset(void)
{
  mlfq_trace_count = 0;
}

void
mlfq_trace_dump(void)
{
  printk("MLFQ_TRACE_BEGIN\n");
  printk("tick pid queue\n");

  uint64 n = mlfq_trace_count;
  if(n > MLFQ_TRACE_SIZE)
    n = MLFQ_TRACE_SIZE;

  for(uint64 i = 0; i < n; i++){
    printk("%ld %d %d\n",
           mlfq_trace[i].tick,
           mlfq_trace[i].pid,
           mlfq_trace[i].queue);
  }

  printk("MLFQ_TRACE_END\n");
}

static void
mlfq_trace_tick(struct proc *p)
{
  if(p == 0)
    return;

  uint64 i = __atomic_fetch_add(&mlfq_trace_count, 1,
                                __ATOMIC_RELAXED);

  if(i < MLFQ_TRACE_SIZE){
    mlfq_trace[i].tick = ticks;
    mlfq_trace[i].pid = p->pid;
    mlfq_trace[i].queue = p->queue;
  }
}


static const int mlfq_quantum[MLFQ_QUEUES] = {1, 4, 8, 16};
static uint64 mlfq_enqueue_counter = 0;

static void
mlfq_enqueue(struct proc *p)
{
  p->enqueue_order =
    __atomic_add_fetch(&mlfq_enqueue_counter, 1, __ATOMIC_RELAXED);
}

/*
 * Move every process to Q0 every 48 ticks.
 */
void
mlfq_boost_all(void)
{
  for(struct proc *p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);

    if(p->state != UNUSED){
      p->queue = 0;
      p->slice_ticks = 0;
      p->boost_ticks = ticks;

      if(p->state == RUNNABLE)
        mlfq_enqueue(p);
    }

    release(&p->lock);
  }
}

/*
 * Account for one timer tick of the currently running process.
 *
 * Return 1 when the process must give up the CPU:
 *   - its current queue's quantum expired, or
 *   - a higher-priority runnable process exists.
 */
int
mlfq_timer_tick(void)
{
  struct proc *p = myproc();

  if(p == 0)
    return 0;

  acquire(&p->lock);

  p->slice_ticks++;
  mlfq_trace_tick(p);

  int preempt = 0;

  if(p->slice_ticks >= mlfq_quantum[p->queue])
    preempt = 1;

  release(&p->lock);

  /*
   * Strict priority: if a higher-priority runnable process
   * exists, the current process gives up the CPU at this
   * timer boundary.
   */
  if(!preempt){
    for(struct proc *q = proc; q < &proc[NPROC]; q++){
      acquire(&q->lock);

      if(q->state == RUNNABLE && q->queue < p->queue)
        preempt = 1;

      release(&q->lock);

      if(preempt)
        break;
    }
  }

  return preempt;
}

/*
 * Timer-driven preemption. A full quantum demotes the process
 * by one level, except Q3 which remains Q3.
 */
void
mlfq_timer_yield(void)
{
  struct proc *p = myproc();

  if(p == 0)
    return;

  acquire(&p->lock);

  if(p->slice_ticks >= mlfq_quantum[p->queue]){
    if(p->queue < MLFQ_QUEUES - 1)
      p->queue++;

    p->slice_ticks = 0;
  }

  p->state = RUNNABLE;
  mlfq_enqueue(p);

  sched();
  release(&p->lock);
}
#endif
#ifdef SCHEDULER_FIFO
  fifo_enqueue(p);
#endif

#ifdef SCHEDULER_FIFO
static uint64 fifo_enqueue_counter = 0;

static void
fifo_enqueue(struct proc *p)
{
  p->enqueue_order =
    __atomic_add_fetch(&fifo_enqueue_counter, 1, __ATOMIC_RELAXED);
}
#endif

int
allocpid()
{
  int pid;

  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc *
allocproc(void)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;

#ifdef SCHEDULER_MLFQ
  p->queue = 0;
  p->slice_ticks = 0;
  p->boost_ticks = ticks;
#endif

#if defined(SCHEDULER_MLFQ) || defined(SCHEDULER_FIFO)
  p->enqueue_order = 0;
#endif

  // Allocate a trapframe page.
  if ((p->trapframe = (struct trapframe *)kalloc()) == 0) {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if (p->pagetable == 0) {
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if (p->trapframe)
    kfree((void *)p->trapframe);
  p->trapframe = 0;
  if (p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if (pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if (mappages(pagetable, TRAMPOLINE, PGSIZE, (uint64)trampoline,
               PTE_R | PTE_X) < 0) {
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if (mappages(pagetable, TRAPFRAME, PGSIZE, (uint64)(p->trapframe),
               PTE_R | PTE_W) < 0) {
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;

  p->cwd = namei("/");
  p->state = RUNNABLE;
#ifdef SCHEDULER_MLFQ
  mlfq_enqueue(p);
#endif
#ifdef SCHEDULER_FIFO
  fifo_enqueue(p);
#endif

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if (n > 0) {
    if (sz + n > TRAPFRAME) {
      return -1;
    }
    if ((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if (n < 0) {
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
kfork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if ((np = allocproc()) == 0) {
    return -1;
  }

  // Copy user memory from parent to child.
  if (uvmcopy(p->pagetable, np->pagetable, p->sz) < 0) {
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for (i = 0; i < NOFILE; i++)
    if (p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
#ifdef SCHEDULER_MLFQ
  np->queue = 0;
  np->slice_ticks = 0;
  np->boost_ticks = ticks;
  mlfq_enqueue(np);
#endif
#ifdef SCHEDULER_FIFO
  fifo_enqueue(np);
#endif
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for (pp = proc; pp < &proc[NPROC]; pp++) {
    if (pp->parent == p) {
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
kexit(int status)
{
  struct proc *p = myproc();

  if (p == initproc)
    panic("init exiting");

  // Close all open files.
  for (int fd = 0; fd < NOFILE; fd++) {
    if (p->ofile[fd]) {
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);

  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
kwait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for (;;) {
    // Scan through table looking for exited children.
    havekids = 0;
    for (pp = proc; pp < &proc[NPROC]; pp++) {
      if (pp->parent == p) {
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if (pp->state == ZOMBIE) {
          // Found one.
          pid = pp->pid;
          if (addr != 0 &&
              copyout(p->pagetable, p->sz, addr, (char *)&pp->xstate,
                      sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          pp->parent = 0;
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || killed(p)) {
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep_prepare(p); //DOC: wait-sleep
    release(&wait_lock);
    sleep();
    acquire(&wait_lock);
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;

  for(;;){
    intr_on();
    intr_off();

#ifdef SCHEDULER_MLFQ
    struct proc *best = 0;

    /*
     * Select the runnable process with:
     *   1. lowest queue number (highest priority)
     *   2. oldest enqueue_order within that queue
     */
    for(p = proc; p < &proc[NPROC]; p++){
      acquire(&p->lock);

      if(p->state == RUNNABLE){
        if(best == 0 ||
           p->queue < best->queue ||
           (p->queue == best->queue &&
            p->enqueue_order < best->enqueue_order)){
          if(best != 0)
            release(&best->lock);

          best = p;
          continue;
        }
      }

      release(&p->lock);
    }

    if(best != 0){
      p = best;

      p->state = RUNNING;
      c->proc = p;

      swtch(&c->context, &p->context);

      mycpu()->intena = 0;
      c->proc = 0;

      release(&p->lock);
    } else {
      asm volatile("wfi");
    }

#elif defined(SCHEDULER_FIFO)
    struct proc *best = 0;

    for(p = proc; p < &proc[NPROC]; p++){
      acquire(&p->lock);

      if(p->state == RUNNABLE){
        if(best == 0 || p->enqueue_order < best->enqueue_order){
          if(best != 0)
            release(&best->lock);

          best = p;
          continue;
        }
      }

      release(&p->lock);
    }

    if(best != 0){
      p = best;

      p->state = RUNNING;
      c->proc = p;

      swtch(&c->context, &p->context);

      mycpu()->intena = 0;
      c->proc = 0;

      release(&p->lock);
    } else {
      asm volatile("wfi");
    }

#else
    int found = 0;

    for(p = proc; p < &proc[NPROC]; p++){
      acquire(&p->lock);

      if(p->state == RUNNABLE){
        p->state = RUNNING;
        c->proc = p;

        swtch(&c->context, &p->context);

        mycpu()->intena = 0;
        c->proc = 0;
        found = 1;
      }

      release(&p->lock);
    }

    if(found == 0)
      asm volatile("wfi");
#endif
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if (!holding(&p->lock))
    panic("sched p->lock");
  if (mycpu()->noff != 1)
    panic("sched locks");
  if (p->state == RUNNING)
    panic("sched RUNNING");
  if (intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();

  acquire(&p->lock);

#ifdef SCHEDULER_MLFQ
  /*
   * Voluntary yield does NOT demote the process.
   * It remains in its current queue and its unused slice
   * is preserved.
   */
  p->state = RUNNABLE;
#ifdef SCHEDULER_MLFQ
        mlfq_enqueue(p);
#endif
#ifdef SCHEDULER_FIFO
  fifo_enqueue(p);
#endif
  mlfq_enqueue(p);
#elif defined(SCHEDULER_FIFO)
    struct proc *best = 0;

    for(p = proc; p < &proc[NPROC]; p++){
      acquire(&p->lock);

      if(p->state == RUNNABLE){
        if(best == 0 || p->enqueue_order < best->enqueue_order){
          if(best != 0)
            release(&best->lock);

          best = p;
          continue;
        }
      }

      release(&p->lock);
    }

    if(best != 0){
      p = best;

      p->state = RUNNING;
      c->proc = p;

      swtch(&c->context, &p->context);

      mycpu()->intena = 0;
      c->proc = 0;

      release(&p->lock);
    } else {
      asm volatile("wfi");
    }

#else
  p->state = RUNNABLE;
#ifdef SCHEDULER_MLFQ
        mlfq_enqueue(p);
#endif
#ifdef SCHEDULER_FIFO
  fifo_enqueue(p);
#endif
#endif

  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  extern char userret[];
  static int first = 1;
  struct proc *p = myproc();

  // Still holding p->lock from scheduler.
  release(&p->lock);

  if (__atomic_load_n(&first, __ATOMIC_ACQUIRE)) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    // ensure other cores see first=0.
    __atomic_store_n(&first, 0, __ATOMIC_RELEASE);

    // We can invoke kexec() now that file system is initialized.
    // Put the return value (argc) of kexec into a0.
    p->trapframe->a0 = kexec("/init", (char *[]){"/init", 0});
    if (p->trapframe->a0 == -1) {
      panic("exec");
    }
  }

  // return to user space, mimicing usertrap()'s return.
  prepare_return();
  uint64 satp = MAKE_SATP(p->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// Register current process as waiting for wakeups on chan.
void
sleep_prepare(void *chan)
{
  struct proc *p = myproc();

  acquire(&p->lock);
  if (chan == 0)
    panic("sleep_prepare: zero chan");
  p->chan = chan;
  release(&p->lock);
}

// Put the thread to sleep.  Assumes sleep_prepare() was called before.
// If the channel registered by sleep_prepare() has been woken up in
// the meantime, do not go to sleep, and instead return immediately.
void
sleep(void)
{
  struct proc *p = myproc();

  acquire(&p->lock);
  if (p->chan != 0) {
    p->state = SLEEPING;
    sched();
  }
  release(&p->lock);
}

// Wake up all processes sleeping on channel chan.
void
wakeup(void *chan)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->chan == chan) {
      // If the process is waiting for wakeups on this channel,
      // signal that the wakeup happened by clearing p->chan.
      p->chan = 0;

      // If this waiting process has gotten so far as to actually
      // go to sleep, also set it back to RUNNING.
      if (p->state == SLEEPING) {
        p->state = RUNNABLE;
#ifdef SCHEDULER_MLFQ
        mlfq_enqueue(p);
#endif
#ifdef SCHEDULER_FIFO
  fifo_enqueue(p);
#endif
      }
    }
    release(&p->lock);
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kkill(int pid)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->pid == pid) {
      p->killed = 1;
      if (p->state == SLEEPING) {
        // Wake process from sleep().
        p->state = RUNNABLE;
#ifdef SCHEDULER_MLFQ
        mlfq_enqueue(p);
#endif
#ifdef SCHEDULER_FIFO
  fifo_enqueue(p);
#endif
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;

  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if (user_dst) {
    return copyout(p->pagetable, p->sz, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if (user_src) {
    return copyin(p->pagetable, p->sz, dst, src, len);
  } else {
    memmove(dst, (char *)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
    [UNUSED]    "unused",
    [USED]      "used",
    [SLEEPING]  "sleep",
    [RUNNABLE]  "runnable",
    [RUNNING]   "run",
    [ZOMBIE]    "zombie"
  };

  printk("PID  NAME                STATE      Q   SLICE BOOST\\n");

  for(struct proc *p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;

#ifdef SCHEDULER_MLFQ
    printk("%d", p->pid);
    if(p->pid < 10) printk("    ");
    else if(p->pid < 100) printk("   ");
    else if(p->pid < 1000) printk("  ");
    else printk(" ");
    printk("%s", p->name);
    int n = strlen(p->name);
    for(int i = n; i < 20; i++) printk(" ");
    printk("%s", states[p->state]);
    n = strlen(states[p->state]);
    for(int i = n; i < 11; i++) printk(" ");
    printk("%d", p->queue);
    printk("   ");
    printk("%d", p->slice_ticks);
    if(p->slice_ticks < 10) printk("     ");
    else if(p->slice_ticks < 100) printk("    ");
    else printk("   ");
    printk("%ld\\n", ticks - p->boost_ticks);
#else
    printk("%d", p->pid);
    if(p->pid < 10) printk("    ");
    else if(p->pid < 100) printk("   ");
    else if(p->pid < 1000) printk("  ");
    else printk(" ");
    printk("%s", p->name);
    int n = strlen(p->name);
    for(int i = n; i < 20; i++) printk(" ");
    printk("%s\\n", states[p->state]);
#endif
  }
}
