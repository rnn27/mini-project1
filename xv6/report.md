# xv6 MLFQ Scheduler Report

## 1. Implementation

The xv6 scheduler was extended with a four-level Multi-Level Feedback Queue (MLFQ):

| Queue | Priority | Time quantum |
|---|---|---:|
| Q0 | Highest | 1 tick |
| Q1 | 2 | 4 ticks |
| Q2 | 3 | 8 ticks |
| Q3 | Lowest | 16 ticks |

Main behaviour:

- New and forked processes enter **Q0**.
- The scheduler always chooses a runnable process from the highest-priority non-empty queue.
- A CPU-bound process is demoted after using its complete quantum.
- A voluntary `yield()` or blocking for I/O does **not** cause demotion.
- Q3 is scheduled round-robin.
- Every **48 ticks**, runnable processes are boosted back to Q0 to prevent starvation.
- Scheduler bookkeeping is exposed through `procdump`.
- `schedulertest` exercises CPU-bound and voluntarily yielding processes.
- `schedtrace` records `(tick, pid, queue)` information for analysis.

## 2. Scheduler Trace

The final single-CPU test produced the following important transitions:

```text
Tick 42-46   : processes begin in Q0
Tick 47-51   : CPU-bound processes start entering Q1
Tick 52-61   : processes continue in Q1
Tick 62-72   : CPU-bound process reaches Q2
Tick 73-95   : PID 10 runs in Q3
Tick 96       : PID 10 is boosted from Q3 back to Q0
Tick 97-105  : PID 10 again moves Q0 -> Q1 -> Q2
```

The full trace is represented in `mlfq_trace.png`.

![MLFQ scheduler trace](mlfq_trace.png)

The trace demonstrates both normal MLFQ demotion and the required periodic priority boost.

## 3. Interpretation

The scheduler gives short or interactive processes access to the CPU quickly by keeping them in higher-priority queues. CPU-bound processes gradually move to lower queues and receive longer time slices, reducing scheduling overhead. The 48-tick boost prevents a long-running process from remaining at a low priority indefinitely. The observed Q3-to-Q0 transition at tick 96 directly confirms the priority-boost mechanism.

## 4. FIFO vs RR vs MLFQ

| Scheduler | Main policy | Advantage | Limitation |
|---|---|---|---|
| FIFO | Oldest runnable process first | Simple and predictable | A long CPU-bound process can delay every process behind it |
| Round Robin | Fixed time quantum | Fair CPU sharing and good response | Does not distinguish interactive and CPU-bound processes |
| MLFQ | Multiple priority queues with dynamic demotion/boosting | Good response for interactive jobs while still sharing CPU with long jobs | More complex bookkeeping and scheduling logic |

For a process:

- **Turnaround time** = completion time − arrival time
- **Waiting time** = turnaround time − CPU burst time
- **Response time** = first CPU start time − arrival time

MLFQ is designed to improve response time for interactive/short jobs compared with FIFO, while avoiding the starvation problem that strict priority scheduling could otherwise create through periodic boosting. Exact turnaround/waiting/response values depend on the selected workload; the submitted trace records scheduling order and queue transitions rather than a complete benchmark table for separate FIFO and RR runs.

## 5. Testing

The implementation was tested with:

```bash
make clean
make SCHEDULER=MLFQ
make SCHEDULER=MLFQ qemu CPUS=1
```

Inside xv6:

```text
schedulertest
schedtrace
procdump
```

The kernel compiled successfully, booted in QEMU, `schedulertest` completed, and the scheduler trace showed Q0 → Q1 → Q2 → Q3 followed by the 48-tick boost back to Q0.

## 6. Conclusion

The xv6 scheduler implements the required MLFQ behaviour with four priority levels, queue-specific quanta, demotion, voluntary-yield preservation, round-robin scheduling at Q3, and periodic priority boosting. The scheduler trace provides direct evidence of the expected queue transitions and priority boost.
