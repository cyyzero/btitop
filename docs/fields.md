# Field support and comparison with procps-ng top

Checked against procps-ng source commit `55b7d01` (`src/top/top.c`, `library/pids.c`) and the local kernel source at `~/linux/mainline/linux` (`fs/proc/array.c`, `fs/proc/task_mmu.c`). The installed procps package is `4.0.4-4ubuntu3.2`.

| Field | procfs / htop-style backend | BPF task iterator | Notes |
| --- | --- | --- | --- |
| PID, USER, PR, NI, state | `/proc/PID/stat`, inode owner | task and cred fields | BPF state combines task and exit state, including idle workers. |
| VIRT | `statm` | `mm->total_vm` | Same page-size conversion. |
| RES, SHR, %MEM | `statm` | `mm->rss_stat` | BPF omits pending per-CPU counter deltas. Shared is file + shmem pages. Kernel threads show zero. |
| %CPU, TIME+ | adjusted `stat` CPU ticks | live task time plus exited-thread signal time | BPF does not apply kernel `thread_group_cputime_adjusted`; small cumulative differences are expected. First %CPU is unavailable. |
| COMMAND | task name from `stat` | `task->comm` | BPF name is limited to 15 bytes. Full command line is read from procfs only for visible rows when requested. |
| CPU, memory, swap, load, uptime | global procfs | global procfs | Both backends share `SystemSource`. |

The default CPU mode follows top's Irix scale. CPU utilization is clamped to 100% per thread (or 100% times thread count for a process) as top does. `I` displays normalized CPU utilization. Thread mode uses each live thread's own CPU counter; process mode includes exited-thread CPU time. For `top` itself, procps-ng uses `CLOCK_BOOTTIME`, fetches `RES` and `SHR` from statm, and samples task CPU ticks, which guided these choices.

The `htop` backend uses the same procfs field parser as `procfs`, but visits each task directory regardless of display mode. In thread view it copies the leader's memory counters to every thread row. This models htop's scan work while retaining btitop's own metric calculations.
