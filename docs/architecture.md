# Architecture

```
BPF iter/task -> binary records --+
                                +--> TaskSource --> Snapshot --> Metrics --> Query --> TUI / text / JSON
procfs / htop-style scan -------+                     ^
SystemSource (/proc global files) --------------------+
TaskControl (pidfd / setpriority) -------------------------------> TUI
```

`TaskSource` returns raw cumulative task counters and field-validity flags. The BPF program writes versioned, fixed-length binary records via `bpf_seq_write`; userspace validates every record and groups live threads in process mode. The procfs backend reads `stat` and `statm`, following top's source. The explicit `htop` backend visits every `/proc/PID/task/TID/stat` even in process view, matching htop's traversal pattern; process CPU comes from the leader's `/proc/PID/stat`, so exited-thread time is retained. Thread rows share the leader's `statm` memory values. `SystemSource` reads global CPU/memory/load files once per sample. `Metrics` uses start time with PID/TID as the task identity, computes deltas, and resets when the backend or thread mode changes. `Query` sorts indices rather than copying task records. The TUI and batch exporters consume the same selected rows.

One sampler thread collects and publishes only the newest snapshot to the UI thread. A slower UI cannot queue up old samples. `CLOCK_BOOTTIME` measures CPU sample intervals as in procps-ng top. A failed BPF load in auto mode selects procfs; a runtime BPF read failure switches source and discards the previous CPU baseline. Explicit BPF mode fails instead.

The task iterator provides a referenced `task_struct` for each callback. Direct CO-RE field reads avoid per-field `bpf_probe_read_kernel` helpers; the command name still uses a bounded kernel string read. The kernel BTF determines field layout at load time. The BPF snapshot is not atomic across all tasks; tasks can exit during an iteration.

Task control is deliberately outside both collection backends. A signal uses `pidfd_open` and `pidfd_send_signal` when available. Renice rechecks the task start time before calling `setpriority`; the small race between recheck and syscall remains because `setpriority` has no pidfd variant. This is reported here rather than presented as a stronger guarantee.
