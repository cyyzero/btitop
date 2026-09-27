# btitop, top, and htop: scenario comparison

Measured on 2026-09-28, Linux `7.0.0-taskstats+`, 16 logical CPUs, procps top `4.0.4-4ubuntu3.2`, htop `3.3.0`. All monitors ran as root so btitop could load BPF. Extra workloads were stopped after each case. CPU seconds are process user+system time; `% of one CPU` is CPU seconds divided by wall seconds. A few tenths of a second can vary with system activity, so treat small differences as inconclusive.

## What each tool enumerates

- Default top calls `procps_pids_reap(..., PIDS_FETCH_TASKS_ONLY)` and reads all visible process leaders; `top -H` uses `PIDS_FETCH_THREADS_TOO`. It sorts the collected tasks and displays only rows that fit the terminal. Batch output confirmed similar row counts to btitop (441 vs 446 process lines, 1944 vs 1947 thread lines including headers and task churn).
- htop's Linux process-table scan enumerates `/proc`, then recurses into each process's `task` directory. Hiding threads affects display, not this scan. Source checked at htop commit `bb3ee0a`, `linux/LinuxProcessTable.c`.
- btitop's `iter/task` BPF program visits kernel tasks, including threads. Process view groups them before display; `--threads` displays separate tasks.

## Batch mode: 12 frames at 100ms

Output was redirected to `/dev/null`; btitop and top each rendered their regular batch text. btitop sample p50 was measured in a separate JSON run under the same workload. The three programs ran sequentially, so tasks can vary slightly.

| Workload | Task rows, median | btitop BPF CPU s | top CPU s | btitop procfs CPU s | BPF sample p50 ms | procfs sample p50 ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Baseline | 435 | 0.09 | 0.15 | 0.24 | 3.41 | 13.67 |
| +200 sleeping processes | 635 | 0.15 | 0.17 | 0.32 | 3.63 | 20.11 |
| +500 sleeping processes | 936 | 0.17 | 0.26 | 0.47 | 4.43 | 28.62 |
| +128 sleeping threads, process view | 436 | 0.10 | 0.16 | 0.26 | 3.15 | 20.33 |
| +128 sleeping threads, thread view | 2068 | 0.35 | 0.54 | 0.99 | 3.09 | 60.56 |
| 4 CPU-busy processes | 440 | 0.09 | 0.12 | 0.22 | 2.79 | 14.88 |
| Rapid short-process creation | 436 | 0.10 | 0.11 | 0.22 | 3.00 | 14.54 |

A longer 30-frame check gave CPU seconds: baseline **0.30 / 0.37 / 0.71**, +500 sleepers **0.50 / 0.59 / 1.13**, and thread view **0.60 / 1.34 / 2.43** for btitop BPF / top / btitop procfs respectively. The small busy/churn differences in the short run are not enough to claim a stable lead.

## Interactive mode: 8 seconds, 1-second refresh

A 110×30 pseudo-terminal was used for each real TUI session. Before the UI fix, btitop sorted and formatted the entire table every 75ms despite sampling once per second: two repeats each used **0.15 CPU seconds**, while top used **0.10**. Redrawing only for new samples, keys, or resize reduced btitop to **0.07 CPU seconds** in two repeats; top used **0.08** in those repeats.

| Workload | btitop BPF CPU s | top CPU s | htop CPU s | btitop peak RSS KiB | top peak RSS KiB | htop peak RSS KiB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Baseline, about 440 processes | 0.06 | 0.10 | 1.36 | 19,108 | 5,464 | 6,888 |
| +500 sleeping processes | 0.08 | 0.21 | 1.64 | 18,904 | 5,280 | 7,772 |

The interactive programs use different layouts and emit different numbers of terminal bytes. These measurements establish observed whole-program cost, not a normalized per-field comparison. btitop has lower CPU cost here but uses roughly 19 MiB peak RSS, versus roughly 5–8 MiB for top and htop. htop's larger cost is consistent with its per-thread procfs scan, though this experiment does not isolate that as the only cause.

## Reproduce

```sh
cmake -S . -B build -G Ninja && cmake --build build
sudo python3 tools/compare_top.py --iterations 12 --interval 0.1 --output /tmp/scenarios.json
sudo python3 tools/compare_interactive.py --seconds 8 --interval 1 --apps btitop top htop
```

Raw results are stored separately with this report. The tests did not cover 10,000 tasks, other kernels, different terminal sizes, or long-running memory behavior. They do not prove btitop will always use less CPU than top; the initial TUI implementation demonstrably used more, and the fix was measured afterward.
