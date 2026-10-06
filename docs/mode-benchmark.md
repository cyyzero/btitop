# Matched process and thread visibility benchmark

This benchmark was rerun on 2026-10-07 on a 16-CPU host running Linux `7.0.0-taskstats+`, procps-ng top 4.0.4, and htop 3.3.0. The same 110×30 pseudo-terminal and refresh interval were used for each interactive program. All monitors ran as root to permit the explicit BPF backend. CPU% is process user plus system CPU time divided by wall time, as a percentage of **one** CPU. Each table entry is the median of three independent runs; individual values remain in the linked JSON files.

The comparison is split into **process view** and **thread view**. In process view, btitop omits `--threads`, top omits `-H`, and htop uses `hide_userland_threads=1`. In thread view, btitop uses `--threads`, top uses `-H`, and htop uses `hide_userland_threads=0`. Both htop configurations explicitly set `hide_kernel_threads=0`, matching btitop's inclusion of kernel tasks. This aligns task visibility, terminal size and refresh cadence; each program still has its own fields, layout, sorting details and collection strategy. Standard top and htop do not accept btitop's `--backend` switch.

`--mode=top|htop` is btitop's procfs scan policy, while `--backend=bpf|procfs` selects its data source. Both BPF modes currently use the same all-task iterator, because process CPU accounting needs the live threads. In process view, top/procfs reads process leaders, whereas htop/procfs reads every `/proc/PID/task/TID/stat` and then displays one process row. In thread view, both procfs policies visit task stat files.

## Workloads and measurement

The first column is **total added processes / total added tasks**, including each process's main thread. The benchmark CLI accepts `extra_sleep_processes:extra_worker_threads`: `0:128` creates one worker process with 128 additional threads, hence **1 / 129**. `100:128` creates 100 sleep processes plus that worker, hence **101 / 229**. The `0 / 0` row adds no synthetic workload; the host already had roughly 430 processes and 1,830 tasks. Every scenario ran three rounds; monitor order rotated within each workload. Workload processes were terminated after each scenario.

The full terminal matrix ran for two seconds per monitor at a 0.2-second refresh. CPU% includes startup, sampling, metric calculation and rendering. Raw results: [process view](../benchmarks/process-thread-scaling/tui-process.json) and [thread view](../benchmarks/process-thread-scaling/tui-threads.json).

### Terminal CPU%, process rows

| Added processes / tasks | btitop top/BPF | top/procfs | htop/BPF | htop/procfs | standard top | standard htop |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 / 0 | 3.76 | 10.92 | 4.26 | 29.42 | 5.31 | 28.05 |
| 100 / 100 | 3.87 | 14.19 | 4.44 | 32.45 | 6.13 | 30.11 |
| 300 / 300 | 4.65 | 18.69 | 4.92 | 34.32 | 8.00 | 33.08 |
| 1 / 129 | 3.87 | 11.41 | 4.46 | 30.84 | 5.56 | 30.17 |
| 1 / 513 | 4.16 | 11.60 | 4.66 | 34.62 | 5.55 | 29.78 |
| 101 / 229 | 4.25 | 13.85 | 4.62 | 33.16 | 6.43 | 30.04 |

### Terminal CPU%, thread rows

| Added processes / tasks | btitop top/BPF | top/procfs | htop/BPF | htop/procfs | standard top | standard htop |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 / 0 | 4.72 | 32.26 | 4.68 | 34.73 | 17.98 | 38.66 |
| 100 / 100 | 4.53 | 34.79 | 4.80 | 37.02 | 18.92 | 40.10 |
| 300 / 300 | 5.24 | 38.00 | 5.24 | 41.51 | 18.56 | 42.00 |
| 1 / 129 | 4.50 | 33.72 | 4.76 | 34.27 | 17.84 | 40.13 |
| 1 / 513 | 5.33 | 40.04 | 5.47 | 38.99 | 20.34 | 42.38 |
| 101 / 229 | 4.99 | 36.24 | 5.14 | 37.42 | 19.95 | 41.27 |

Hiding thread rows reduces standard htop's cost markedly. Its process-view CPU% changes little between baseline and 512 added threads because htop skips much per-thread field work for hidden rows. btitop's htop/procfs mode still reads every thread stat in process view, so its cost rises more. The [htop 3.3.0 process-table code](https://github.com/htop-dev/htop/blob/3.3.0/linux/LinuxProcessTable.c) explains the different traversal and skip behavior; a [later revision](https://github.com/htop-dev/htop/blob/bb3ee0a/linux/LinuxProcessTable.c) also removes a redundant nested task-directory attempt in this installed version. These are different implementations even when they show the same classes of task.

## Default one-second refresh

To reduce startup's share, each monitor also ran for eight seconds at the default one-second refresh under baseline and 512 added threads. Both process and thread views ran three rounds. These top and htop triples were measured sequentially rather than simultaneously. Raw results: [top process](../benchmarks/process-thread-scaling/top-default-process.json), [top threads](../benchmarks/process-thread-scaling/top-default-threads.json), [htop process](../benchmarks/process-thread-scaling/htop-default-process.json), and [htop threads](../benchmarks/process-thread-scaling/htop-default-threads.json).

| View | Added processes / tasks | btitop top/BPF | top/procfs | standard top | btitop htop/BPF | htop/procfs | standard htop |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Processes | 0 / 0 | 1.02 | 2.48 | 1.32 | 0.99 | 7.64 | 11.73 |
| Processes | 1 / 513 | 1.10 | 2.48 | 1.36 | 1.08 | 8.81 | 12.29 |
| Threads | 0 / 0 | 1.17 | 9.42 | 5.47 | 1.21 | 9.17 | 16.16 |
| Threads | 1 / 513 | 1.37 | 11.59 | 6.83 | 1.29 | 11.53 | 18.62 |

For the standard htop process view, the three runs ranged from 10.98–12.42% at baseline and 11.64–13.34% with 512 additional threads. In thread view they ranged from 14.51–17.00% and 18.14–19.72% respectively. Thus the difference between its two visibility settings is larger than this run-to-run spread. The btitop htop/BPF medians were 0.99–1.29% across these four matched cases, but its displayed fields and terminal output are not identical to standard htop's.

## Collection and JSON path

The JSON benchmark takes six samples at a 0.2-second interval. Its CPU% includes collection, metric calculation, row selection and JSON serialization, but no terminal rendering. `sample_ms_p50` measures global-stat reading through task collection, excluding subsequent calculation and output. Raw results: [process view](../benchmarks/process-thread-scaling/scan-process.json) and [thread view](../benchmarks/process-thread-scaling/scan-threads.json).

### JSON CPU%, process rows

| Added processes / tasks | top/BPF | top/procfs | htop/BPF | htop/procfs |
| --- | ---: | ---: | ---: | ---: |
| 0 / 0 | 5.25 | 11.95 | 6.30 | 31.98 |
| 100 / 100 | 5.74 | 14.46 | 6.64 | 32.13 |
| 300 / 300 | 6.21 | 16.06 | 6.40 | 36.56 |
| 1 / 129 | 5.27 | 12.83 | 6.43 | 32.22 |
| 1 / 513 | 5.70 | 12.92 | 6.71 | 33.63 |
| 101 / 229 | 5.77 | 14.62 | 6.86 | 37.73 |

### JSON CPU%, thread rows

| Added processes / tasks | top/BPF | top/procfs | htop/BPF | htop/procfs |
| --- | ---: | ---: | ---: | ---: |
| 0 / 0 | 7.05 | 34.95 | 7.94 | 37.69 |
| 100 / 100 | 7.76 | 38.32 | 7.79 | 38.34 |
| 300 / 300 | 8.19 | 41.16 | 7.64 | 43.88 |
| 1 / 129 | 8.21 | 38.96 | 8.39 | 39.44 |
| 1 / 513 | 8.38 | 44.23 | 8.36 | 42.36 |
| 101 / 229 | 8.24 | 38.45 | 8.46 | 41.62 |

At baseline, collection p50 medians in process view were 3.04 ms (top/BPF), 19.00 ms (top/procfs), 3.21 ms (htop/BPF), and 49.28 ms (htop/procfs). In thread view they were 2.36, 55.66, 2.51, and 56.93 ms respectively. The BPF paths remain lower on this host, but these numbers are not a guaranteed speedup on other machines. The two-second terminal window includes substantial startup work; background tasks changed between runs; the programs render different fields; and 10,000-task and other-kernel cases remain untested.

## Reproduce

```sh
cmake -S . -B build -G Ninja
cmake --build build
for view in process threads; do
  sudo python3 tools/compare_modes.py --display "$view" --rounds 3 --iterations 6 --interval 0.2 --output "benchmarks/process-thread-scaling/scan-$view.json"
  sudo python3 tools/compare_modes.py --display "$view" --rounds 3 --iterations 6 --interval 0.2 --matched-tui-all --reference-seconds 2 --output "benchmarks/process-thread-scaling/tui-$view.json"
  sudo python3 tools/compare_modes.py --display "$view" --scenarios 0:0,0:512 --rounds 3 --iterations 8 --interval 1 --top-pair-only --reference-seconds 8 --output "benchmarks/process-thread-scaling/top-default-$view.json"
  sudo python3 tools/compare_modes.py --display "$view" --scenarios 0:0,0:512 --rounds 3 --iterations 8 --interval 1 --htop-pair-only --reference-seconds 8 --output "benchmarks/process-thread-scaling/htop-default-$view.json"
done
```

Run the commands sequentially so monitors do not perturb one another. The earlier unmatched comparison and syscall traces are retained in [`legacy`](../benchmarks/process-thread-scaling/legacy/) for historical inspection and are not used in the tables above.
