#include "btitop/wire.h"
#include "vmlinux.h"
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_helpers.h>
char LICENSE[] SEC("license") = "GPL";

SEC("iter/task")
int collect_task(struct bpf_iter__task *ctx) {
    struct task_struct *task = ctx->task;
    if (!task)
        return 0;
    struct btitop_wire_task out = {};
    out.version = BTITOP_WIRE_VERSION;
    out.length = sizeof(out);
    out.tid = task->pid;
    out.pid = task->tgid;
    if (!out.tid || !out.pid)
        return 0;
    out.flags = out.tid == out.pid ? BTITOP_FLAG_LEADER : 0;
    struct task_struct *parent = task->real_parent;
    if (parent)
        out.ppid = parent->tgid;
    const struct cred *cred = task->cred;
    if (cred)
        out.uid = cred->uid.val;
    out.state = task->__state | task->exit_state;
    out.priority = task->prio - 100;
    out.nice = task->static_prio - 120;
    out.start_ns = task->start_boottime;
    out.cpu_ns = task->utime + task->stime;
    struct signal_struct *sig = task->signal;
    if (sig && (out.flags & BTITOP_FLAG_LEADER)) {
        out.threads = sig->nr_threads;
        out.dead_cpu_ns = sig->utime + sig->stime;
    }
    struct mm_struct *mm = task->mm;
    if (mm) {
        out.flags |= BTITOP_FLAG_MM;
        out.virt_bytes = mm->total_vm;
        long file = mm->rss_stat[0].count;
        long anon = mm->rss_stat[1].count;
        long shmem = mm->rss_stat[3].count;
        if (file < 0)
            file = 0;
        if (anon < 0)
            anon = 0;
        if (shmem < 0)
            shmem = 0;
        out.shared_pages = file + shmem;
        out.rss_pages = file + anon + shmem;
    }
    bpf_probe_read_kernel_str(out.comm, sizeof(out.comm), task->comm);
    bpf_seq_write(ctx->meta->seq, &out, sizeof(out));
    return 0;
}
