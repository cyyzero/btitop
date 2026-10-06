# btitop

A Linux terminal task monitor using a BPF task iterator, with procfs and htop-style scanning backends. The collection, metric, query, and presentation layers are independent. `btitop` defaults to one sample per second and focuses on low monitoring overhead.

![btitop running in a terminal](assets/screenshot.png)

## Build

Requirements: C++20 compiler, CMake ≥3.20, Clang with BPF target, libbpf development files, libelf, ncursesw, bpftool, and kernel BTF at `/sys/kernel/btf/vmlinux`. On Ubuntu: `sudo apt install clang libbpf-dev libelf-dev libncurses-dev cmake ninja-build linux-tools-common`. Some custom kernels have a `bpftool` wrapper that cannot find its binary; pass `-DBPFTOOL_EXECUTABLE=/path/to/real/bpftool` to CMake.

```sh
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
./build/btitop
```

Install the binary and BPF object together with `sudo cmake --install build`; set `BTITOP_BPF_OBJECT` to an alternate object path if needed.

BPF loading normally requires elevated privileges or suitable BPF capabilities. The default `auto` backend reports a failed BPF load and uses procfs. To require BPF, run `sudo ./build/btitop --backend=bpf`. No permanent capabilities or privileged service are installed.

## Usage

```sh
./build/btitop --backend=procfs
./build/btitop --mode=htop --backend=procfs
sudo ./build/btitop --mode=htop --backend=bpf
./build/btitop --mode=htop --backend=procfs --threads
sudo ./build/btitop --backend=bpf --threads
./build/btitop --json --iterations=2 --interval=1
./build/btitop --batch --pid=1234 --sort=mem
```

Options: `--mode=top|htop`, `--backend=auto|bpf|procfs`, `--interval=SECONDS` (0.1–3600), `--pid=PID`, `--user=NAME|UID`, `--threads`, `--sort=cpu|mem|pid|time|name`, `--batch`, `--json`, `--iterations=N`, `--no-color`. The mode selects a scan policy and the backend selects a data source. With procfs, `htop` always enumerates `/proc/PID/task`, including when only process rows are displayed; `top` reads process leaders unless thread view is enabled. With BPF, both modes currently use the same `iter/task` scan of all threads, needed to assemble process CPU correctly. Thus the BPF mode pair is a useful CLI comparison, but it does not represent two different BPF algorithms. The header and JSON identify both mode and backend and distinguish `scanned_tasks` from displayed rows. The former `--backend=htop` spelling remains an alias for `--mode=htop --backend=procfs`. `auto` selects BPF or the procfs scan policy for the selected mode.

Keys: `q` quit, arrows/`j` and `K` move, `h` help, `t` threads, `c` command line, `/` search, `u` UID filter, `p` PID filter, `s` sort, `R` reverse, `z` tree, `f` choose columns (comma-separated names), `I` normalized CPU, `1` per-core CPU, `+`/`-` interval, Enter details, `k` signal, `r` renice. Signal and renice require typing `YES` after choosing the target and parameter. Display settings are saved under `$XDG_CONFIG_HOME/btitop/config` (or `~/.config/btitop/config`).

## Data semantics

First-sample `%CPU` is `N/A`; later samples use task CPU-time delta divided by elapsed `CLOCK_BOOTTIME` time. Default Irix mode allows a multithreaded process to exceed 100%; `I` divides by online CPU count. Task identity combines PID/TID and start time to prevent PID-reuse errors. BPF process CPU includes accumulated exited-thread time plus live-thread time. The kernel's procfs path applies additional CPU-time adjustment, so BPF `TIME+` may be slightly lower and is not bit-for-bit equal to top. The BPF RSS/SHR counters can differ slightly from procfs because per-CPU memory counters have pending deltas. See [field matrix](docs/fields.md).

The BPF sampling path does not scan per-task procfs files. The TUI reads `/proc/PID/cmdline` only for visible rows when command-line mode is enabled, with a bounded cache. Global system statistics come from `/proc/stat`, `/proc/meminfo`, `/proc/loadavg`, and `/proc/uptime`. Short-lived tasks can be missed between snapshots.

## Performance and architecture

[Architecture](docs/architecture.md) describes the data flow. [Benchmarks](docs/benchmarks.md) records reproducible measurements and perf findings. [Mode/backend matrix](docs/mode-benchmark.md) compares all four combinations and standard top/htop across process and thread counts. [Scenario comparison](docs/comparison.md) contains earlier measurements. `tools/compare_modes.py` reproduces the matrix with BPF loading privileges; `tools/benchmark.py` remains a quick three-path collection check.

Current builds target x86-64 and arm64 Linux. The BPF program is built against the current kernel's BTF and uses CO-RE relocations. Linux 6.6 and 6.12 compatibility remains to be verified on those kernels; a successful build on this host does not establish that compatibility.
