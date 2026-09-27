#include "btitop/core.hpp"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <mutex>
#include <pwd.h>
#include <sstream>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
namespace btitop {
size_t IdentityHash::operator()(const Identity &x) const noexcept {
    return (static_cast<uint64_t>(x.pid) << 32) ^ (static_cast<uint64_t>(x.tid) << 16) ^ x.start_ns;
}
uint64_t CpuTimes::total() const {
    return user + nice + system + idle + iowait + irq + softirq + steal;
}
uint64_t CpuTimes::busy() const {
    return total() - idle - iowait;
}
void Metrics::reset() {
    last_.clear();
    next_.clear();
    last_cpu_.clear();
    last_time_ = {};
    last_boot_ns_ = 0;
    backend_.clear();
}
void Metrics::apply(Snapshot &s, bool normalize) {
    if (backend_ != s.backend)
        reset();
    const double elapsed = s.boottime_ns && last_boot_ns_ && s.boottime_ns >= last_boot_ns_
                               ? (s.boottime_ns - last_boot_ns_) / 1e9
                               : (last_time_ == Clock::time_point{}
                                      ? 0
                                      : std::chrono::duration<double>(s.end - last_time_).count());
    next_.clear();
    next_.reserve(s.tasks.size() * 2 + 1);
    const double cores =
        std::max<size_t>(1, s.system.cpus.size() > 0 ? s.system.cpus.size() - 1 : 1);
    for (auto &t : s.tasks) {
        t.cpu_percent.reset();
        t.mem_percent.reset();
        if (auto it = last_.find(t.id);
            it != last_.end() && elapsed > 0 && t.cpu_ns >= it->second) {
            double pct = (t.cpu_ns - it->second) * 100.0 / (elapsed * 1e9);
            pct = std::min(pct, 100.0 * std::max(1, s.thread_mode ? 1 : t.threads));
            t.cpu_percent = normalize ? pct / cores : pct;
        }
        if (s.system.memory_total && t.memory_valid)
            t.mem_percent = t.rss_bytes * 100.0 / s.system.memory_total;
        next_.emplace(t.id, t.cpu_ns);
    }
    last_.swap(next_);
    s.system.cpu_percent.resize(s.system.cpus.size());
    if (last_cpu_.size() == s.system.cpus.size())
        for (size_t i = 0; i < s.system.cpus.size(); ++i) {
            auto a = last_cpu_[i], b = s.system.cpus[i];
            if (b.total() > a.total() && b.busy() >= a.busy())
                s.system.cpu_percent[i] = 100.0 * (b.busy() - a.busy()) / (b.total() - a.total());
        }
    last_cpu_ = s.system.cpus;
    last_time_ = s.end;
    last_boot_ns_ = s.boottime_ns;
    backend_ = s.backend;
}
std::vector<size_t> select(const std::vector<Task> &tasks, const Query &q) {
    std::vector<size_t> ids;
    ids.reserve(tasks.size());
    for (size_t i = 0; i < tasks.size(); ++i) {
        const auto &t = tasks[i];
        if (q.pid && t.id.pid != *q.pid && t.id.tid != *q.pid)
            continue;
        if (q.uid && t.uid != *q.uid)
            continue;
        if (!q.search.empty() && t.name.find(q.search) == std::string::npos &&
            t.command.find(q.search) == std::string::npos &&
            std::to_string(t.id.tid).find(q.search) == std::string::npos)
            continue;
        ids.push_back(i);
    }
    auto less = [&](size_t a, size_t b) {
        const auto &x = tasks[a];
        const auto &y = tasks[b];
        switch (q.sort) {
        case Sort::CPU: {
            double u = x.cpu_percent.value_or(-1), v = y.cpu_percent.value_or(-1);
            if (u != v)
                return q.descending ? u > v : u < v;
            break;
        }
        case Sort::MEMORY:
            if (x.rss_bytes != y.rss_bytes)
                return q.descending ? x.rss_bytes > y.rss_bytes : x.rss_bytes < y.rss_bytes;
            break;
        case Sort::PID:
            if (x.id.tid != y.id.tid)
                return q.descending ? x.id.tid > y.id.tid : x.id.tid < y.id.tid;
            break;
        case Sort::TIME:
            if (x.cpu_ns != y.cpu_ns)
                return q.descending ? x.cpu_ns > y.cpu_ns : x.cpu_ns < y.cpu_ns;
            break;
        case Sort::NAME:
            if (x.name != y.name)
                return q.descending ? x.name > y.name : x.name < y.name;
            break;
        }
        return x.id.tid < y.id.tid;
    };
    std::sort(ids.begin(), ids.end(), less);
    if (q.tree) {
        std::unordered_map<int, std::vector<size_t>> children;
        std::unordered_map<int, size_t> located;
        for (auto i : ids)
            located[tasks[i].id.tid] = i;
        std::vector<size_t> roots;
        for (auto i : ids) {
            auto p = tasks[i].ppid;
            if (p != tasks[i].id.tid && located.contains(p))
                children[p].push_back(i);
            else
                roots.push_back(i);
        }
        std::vector<size_t> ordered;
        ordered.reserve(ids.size());
        auto walk = [&](auto &&self, size_t i, int depth) -> void {
            if (depth > 64)
                return;
            ordered.push_back(i);
            for (auto child : children[tasks[i].id.tid])
                self(self, child, depth + 1);
        };
        for (auto i : roots)
            walk(walk, i, 0);
        ids = std::move(ordered);
    }
    return ids;
}
std::string state_name(char s) {
    switch (s) {
    case 'R':
        return "running";
    case 'S':
        return "sleeping";
    case 'D':
        return "disk sleep";
    case 'T':
        return "stopped";
    case 'Z':
        return "zombie";
    case 'I':
        return "idle";
    default:
        return "other";
    }
}
std::string format_bytes(uint64_t b) {
    const char *units[] = {"B", "K", "M", "G", "T"};
    double n = b;
    int i = 0;
    while (n >= 1024 && i < 4) {
        n /= 1024;
        ++i;
    }
    std::ostringstream o;
    o << std::fixed << std::setprecision(i ? 1 : 0) << n << units[i];
    return o.str();
}
std::string format_time(uint64_t ns) {
    uint64_t cs = ns / 10000000;
    std::ostringstream o;
    o << cs / 6000 << ':' << std::setfill('0') << std::setw(2) << (cs / 100) % 60 << '.'
      << std::setw(2) << cs % 100;
    return o.str();
}
std::string username(int uid) {
    static std::mutex mu;
    static std::unordered_map<int, std::string> cache;
    std::scoped_lock lock(mu);
    if (auto it = cache.find(uid); it != cache.end())
        return it->second;
    struct passwd p {
    }, *result = nullptr;
    char buf[4096];
    std::string value = std::to_string(uid);
    if (!getpwuid_r(uid, &p, buf, sizeof(buf), &result) && result)
        value = p.pw_name;
    if (cache.size() > 4096)
        cache.clear();
    return cache.emplace(uid, value).first->second;
}
std::string command_line(int pid) {
    std::string path = "/proc/" + std::to_string(pid) + "/cmdline";
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return {};
    char buf[512];
    ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);
    if (n <= 0)
        return {};
    for (ssize_t i = 0; i < n; i++)
        if (buf[i] == '\0')
            buf[i] = ' ';
    return std::string(buf, n);
}
} // namespace btitop
