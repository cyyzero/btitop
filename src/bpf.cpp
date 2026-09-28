#include "btitop/core.hpp"
#include "btitop/wire.h"
#include <algorithm>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <linux/limits.h>
#include <unistd.h>
#include <unordered_map>
namespace btitop {
namespace {
char state(uint32_t bits) {
    if (bits == 0)
        return 'R';
    if ((bits & 1026) == 1026)
        return 'I';
    if (bits & 1)
        return 'S';
    if (bits & 2)
        return 'D';
    if (bits & 4)
        return 'T';
    if (bits & 8)
        return 't';
    if (bits & 16)
        return 'X';
    if (bits & 32)
        return 'Z';
    if (bits & 64)
        return 'P';
    return '?';
}
class BpfSource final : public TaskSource {
    bpf_object *obj_{};
    bpf_link *link_{};
    std::vector<char> bytes_;
    std::unordered_map<int, uint64_t> dead_;
    std::unordered_map<int, size_t> leaders_;
    size_t scanned_{};

  public:
    BpfSource(bpf_object *o, bpf_link *l) : obj_(o), link_(l) {
    }
    ~BpfSource() override {
        if (link_)
            bpf_link__destroy(link_);
        if (obj_)
            bpf_object__close(obj_);
    }
    std::string name() const override {
        return "bpf";
    }
    size_t scanned_tasks() const override {
        return scanned_;
    }
    bool collect(std::vector<Task> &out, bool threads, std::string &error) override {
        int fd = bpf_iter_create(bpf_link__fd(link_));
        if (fd < 0) {
            error = "bpf_iter_create: " + std::string(strerror(errno));
            return false;
        }
        bytes_.clear();
        bytes_.reserve(1 << 20);
        char chunk[65536];
        ssize_t n;
        bool ok = true;
        while ((n = read(fd, chunk, sizeof(chunk))) > 0) {
            if (bytes_.size() + n > 128 * 1024 * 1024) {
                error = "iterator output exceeds 128 MiB";
                ok = false;
                break;
            }
            bytes_.insert(bytes_.end(), chunk, chunk + n);
        }
        if (n < 0) {
            error = "iterator read: " + std::string(strerror(errno));
            ok = false;
        }
        close(fd);
        if (!ok)
            return false;
        if (bytes_.size() % sizeof(btitop_wire_task)) {
            error = "truncated BPF record";
            return false;
        }
        scanned_ = bytes_.size() / sizeof(btitop_wire_task);
        out.clear();
        out.reserve(bytes_.size() / sizeof(btitop_wire_task));
        dead_.clear();
        for (size_t pos = 0; pos < bytes_.size(); pos += sizeof(btitop_wire_task)) {
            btitop_wire_task w{};
            memcpy(&w, bytes_.data() + pos, sizeof(w));
            if (w.version != BTITOP_WIRE_VERSION || w.length != sizeof(w)) {
                error = "unsupported BPF record format";
                return false;
            }
            Task t;
            t.id = {static_cast<int>(w.pid), static_cast<int>(w.tid), w.start_ns};
            t.ppid = w.ppid;
            t.uid = w.uid;
            t.priority = w.priority;
            t.nice = w.nice;
            t.threads = w.threads ? w.threads : 1;
            t.state = state(w.state);
            t.cpu_ns = w.cpu_ns;
            t.virtual_bytes = w.virt_bytes * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
            t.rss_bytes = w.rss_pages * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
            t.shared_bytes = w.shared_pages * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
            t.memory_valid = true;
            t.name = std::string(w.comm, strnlen(w.comm, sizeof(w.comm)));
            if (w.flags & BTITOP_FLAG_LEADER)
                dead_[t.id.pid] = w.dead_cpu_ns;
            out.push_back(std::move(t));
        }
        if (!threads) {
            leaders_.clear();
            leaders_.reserve(out.size());
            for (size_t i = 0; i < out.size(); ++i)
                if (out[i].id.pid == out[i].id.tid)
                    leaders_[out[i].id.pid] = i;
            for (auto &t : out)
                if (t.id.pid != t.id.tid)
                    if (auto it = leaders_.find(t.id.pid); it != leaders_.end())
                        out[it->second].cpu_ns += t.cpu_ns;
            size_t write = 0;
            for (size_t i = 0; i < out.size(); ++i) {
                if (out[i].id.pid != out[i].id.tid)
                    continue;
                out[i].cpu_ns += dead_[out[i].id.pid];
                if (write != i)
                    out[write] = std::move(out[i]);
                ++write;
            }
            out.resize(write);
        }
        return true;
    }
};
} // namespace
std::unique_ptr<TaskSource> make_bpf_source(std::string &error) {
    const char *override = std::getenv("BTITOP_BPF_OBJECT");
    std::string object = override && *override ? override : BTITOP_BPF_OBJECT;
    if (!(override && *override) && !std::filesystem::exists(object))
        object = BTITOP_INSTALLED_BPF_OBJECT;
    bpf_object *obj = bpf_object__open_file(object.c_str(), nullptr);
    if (!obj || libbpf_get_error(obj)) {
        error = "cannot open BPF object at " + object;
        return nullptr;
    }
    if (int rc = bpf_object__load(obj); rc) {
        error = "BPF load failed: " + std::string(strerror(-rc));
        bpf_object__close(obj);
        return nullptr;
    }
    auto *prog = bpf_object__find_program_by_name(obj, "collect_task");
    if (!prog) {
        error = "BPF task iterator program missing";
        bpf_object__close(obj);
        return nullptr;
    }
    auto *link = bpf_program__attach_iter(prog, nullptr);
    if (!link || libbpf_get_error(link)) {
        error = "BPF iterator attach failed";
        bpf_object__close(obj);
        return nullptr;
    }
    return std::make_unique<BpfSource>(obj, link);
}
} // namespace btitop
