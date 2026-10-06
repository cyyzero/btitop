# Performance measurements

For current comparisons with standard top and htop under matched process/thread visibility, see the [mode/backend benchmark](mode-benchmark.md). The measurements below are earlier profiling runs and should not be combined with its CPU percentages.

Environment: x86-64, Linux `7.0.0-taskstats+`, approximately 425–453 tasks, procps `4.0.4-4ubuntu3.2`. These are observations from one host, not a claim about other kernels or task counts.

Reproduce the collection comparison:

```sh
sudo python3 tools/benchmark.py --binary build/btitop --iterations 30 --interval 0.1
sudo /usr/bin/time -f 'top cpu=%U+%S rss=%M' top -b -n 30 -d 0.1 -w 120 >/dev/null
```

| Run | Tasks | CPU seconds, 30 samples | Sample p50 | Sample p95 |
| --- | ---: | ---: | ---: | ---: |
| btitop BPF (after direct CO-RE reads and buffer reuse) | 437 | 0.183 | 3.205 ms | 3.938 ms |
| btitop procfs | 437 | 0.522 | 14.382 ms | 19.51 ms |
| procps top batch | roughly 437 | 0.33 | — | — |

An additional 500 sleeping processes gave 939 total tasks: BPF p50/p95 3.49/3.96 ms and 0.103 CPU seconds over 12 samples; procfs p50/p95 28.434/28.691 ms and 0.385 CPU seconds. The benchmark terminated all spawned processes. A 100-sample, 100ms BPF batch run exited normally; observed RSS rose during startup and reached 6,596 KiB by the end. This short run does not establish long-term memory stability.

The btitop runs exported JSON to a pipe and parsed it in the benchmark process; top output was discarded. These are related workloads, not identical rendering costs. Earlier, a 12-sample run with helper-based BPF reads measured 0.113 CPU seconds and 4.285 ms median at approximately 425 tasks. The task counts and run lengths differ, so those CPU totals should not be compared directly.

`perf record` on 50 BPF samples initially showed `copy_from_kernel_nofault` 9.79%, BPF program 5.23%, and `bpf_probe_read_kernel` 4.55% of samples. After direct CO-RE reads, the latter two helper hotspots disappeared from the top entries; `bpf_iter_run_prog` was 3.99%, BPF program 3.71%, and kernel string copy 2.67%. The procfs profile was dominated by per-task procfs activity and C++ stream parsing, including `pid_revalidate` and `do_task_stat`.

To profile again, use the real perf binary for the running kernel when the distro wrapper fails, for example `/usr/lib/linux-tools/7.0.0-31-generic/perf`. For larger tests, `tools/benchmark.py --spawn=N` creates `N` sleeping processes and terminates them after both runs. Start with a value allowed by the host's PID and memory limits. The 10,000-task acceptance case, long-running RSS stability, and standard Linux 6.6/6.12 runs remain unverified.
