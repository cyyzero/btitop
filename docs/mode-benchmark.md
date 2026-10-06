# Mode/backend benchmark — 2026-10-07

This experiment separates the **scan policy** (`--mode=top|htop`) from the **data source** (`--backend=bpf|procfs`). Standard top and htop are separate reference programs; neither accepts a btitop backend choice. All measurements were made on one 16-CPU host running Linux `7.0.0-taskstats+`, procps-ng top 4.0.4, and htop 3.3.0. The processes ran as root so BPF could load.

In process view, top/procfs reads process leaders, while htop/procfs reads every `/proc/PID/task/TID/stat` and still shows one process row. Both BPF mode selections run the **same** all-task iterator; live thread CPU must be included to calculate process CPU. Therefore the two BPF columns are independent repetitions of one implementation, not distinct algorithms.

## Controlled workload and measurement

The six scenarios added 0, 100, or 300 sleeping processes; 0, 128, or 512 sleeping threads in one process; and one mixed case. The host already had about 423–425 process rows and about 1,618–1,622 tasks at baseline. Every scenario ran three rounds. The four btitop combinations rotated order across rounds; top and htop alternated order. Workload children were terminated after each scenario. `CPU%` means process user + system CPU time divided by wall time, as a fraction of **one** CPU; the tables give the median of three runs. Raw per-run values are in [`benchmarks/2026-10-07`](../benchmarks/2026-10-07/).

The first test used six JSON samples at a 0.2-second interval. Its CPU% includes collection, metric calculation, selection and JSON serialization. Sample p50 is the measured time from the beginning of global-stat reading through task collection; it excludes metric calculation and output. All four combinations emitted process rows, so the JSON row count is comparable.

| Added processes / threads | top/BPF CPU% | top/procfs CPU% | htop/BPF CPU% | htop/procfs CPU% |
| --- | ---: | ---: | ---: | ---: |
| 0 / 0 | 4.73 | 9.63 | 4.88 | 25.68 |
| 100 / 0 | 4.78 | 11.80 | 4.98 | 28.08 |
| 300 / 0 | 5.51 | 15.39 | 5.56 | 33.56 |
| 0 / 128 | 4.48 | 9.53 | 5.18 | 29.65 |
| 0 / 512 | 4.87 | 9.86 | 5.00 | 33.91 |
| 100 / 128 | 5.11 | 11.61 | 5.35 | 30.11 |

| Added processes / threads | top/BPF p50 ms | top/procfs p50 ms | htop/BPF p50 ms | htop/procfs p50 ms |
| --- | ---: | ---: | ---: | ---: |
| 0 / 0 | 2.39 | 14.09 | 2.21 | 42.41 |
| 100 / 0 | 2.19 | 17.75 | 2.38 | 46.12 |
| 300 / 0 | 2.82 | 22.84 | 2.64 | 56.59 |
| 0 / 128 | 2.45 | 13.79 | 2.74 | 46.48 |
| 0 / 512 | 3.13 | 14.47 | 3.03 | 54.93 |
| 100 / 128 | 2.67 | 16.93 | 2.87 | 50.41 |

## Actual terminal programs

Each program then ran in a 110×30 pseudo-terminal for two seconds with a 0.2-second refresh. The table compares whole-program CPU%, including startup and rendering. The btitop modes use the same screen layout; standard top and htop have different fields and terminal output, so their figures are reference costs rather than normalized per-field comparisons.

| Added processes / threads | btitop top/BPF | btitop top/procfs | btitop htop/BPF | btitop htop/procfs | standard top | standard htop |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 / 0 | 3.82 | 13.65 | 4.29 | 28.54 | 5.22 | 32.83 |
| 100 / 0 | 4.02 | 14.76 | 4.16 | 30.07 | 6.16 | 34.23 |
| 300 / 0 | 4.37 | 19.12 | 4.76 | 33.23 | 7.04 | 38.45 |
| 0 / 128 | 3.86 | 13.83 | 4.36 | 30.16 | 5.01 | 36.25 |
| 0 / 512 | 3.89 | 11.58 | 3.93 | 30.08 | 5.13 | 41.34 |
| 100 / 128 | 3.75 | 12.89 | 3.73 | 29.15 | 6.01 | 37.33 |

The two-second window gives startup work a large share. As a check at the project's default **one-second refresh**, each program ran for eight seconds under baseline and +512 threads. These longer-window results are single runs, not three-round medians:

| Added processes / threads | btitop top/BPF | btitop top/procfs | btitop htop/BPF | btitop htop/procfs | standard top | standard htop |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 0 / 0 | 0.81 | 2.20 | 0.96 | 7.01 | 1.31 | 13.60 |
| 0 / 512 | 1.07 | 2.49 | 0.99 | 9.05 | 1.26 | 16.79 |

The observed pattern is consistent: the process-only procfs path scales with process count, the htop-style procfs path also scales with thread count, and BPF remains low in these cases despite scanning all tasks. This does not prove a fixed speedup on other machines. Background tasks changed slightly between runs, the sample is small, the TUI programs render different amounts of data, and the 10,000-task and other-kernel cases remain untested.

## Reproduce

```sh
cmake -S . -B build -G Ninja
cmake --build build
sudo python3 tools/compare_modes.py --rounds 3 --iterations 6 --interval 0.2 --output scan-matrix.json
sudo python3 tools/compare_modes.py --rounds 3 --iterations 6 --interval 0.2 --tui-matrix-only --reference-seconds 2 --output btitop-tui-matrix.json
sudo python3 tools/compare_modes.py --rounds 3 --iterations 6 --interval 0.2 --references-only --reference-seconds 2 --output reference-tui-matrix.json
```

The benchmark accepts `--scenarios` as comma-separated `processes:threads` pairs. Use `--interval 1 --reference-seconds 8` for the longer TUI check. Run the terminal benchmarks separately, since concurrent monitors would interfere with the CPU measurements.
