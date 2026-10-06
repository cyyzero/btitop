#include "btitop/core.hpp"
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <linux/limits.h>
#include <sstream>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
namespace btitop {
namespace {
uint64_t hz() {
    static auto v = static_cast<uint64_t>(sysconf(_SC_CLK_TCK));
    return v;
}
uint64_t page() {
    static auto v = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
    return v;
}
bool decimal(const char *s) {
    if (!s || !*s)
        return false;
    for (; *s; ++s)
        if (*s < '0' || *s > '9')
            return false;
    return true;
}
bool read_stat(const std::string &path, Task &t) {
    std::ifstream f(path);
    std::string line;
    if (!std::getline(f, line))
        return false;
    auto l = line.find('('), r = line.rfind(')');
    if (l == std::string::npos || r == std::string::npos || r <= l)
        return false;
    t.name = line.substr(l + 1, r - l - 1);
    std::istringstream ss(line.substr(r + 2));
    std::vector<std::string> v;
    std::string part;
    while (ss >> part)
        v.push_back(part);
    if (v.size() < 22)
        return false;
    try {
        t.state = v[0][0];
        t.ppid = std::stoi(v[1]);
        t.cpu_ns = (std::stoull(v[11]) + std::stoull(v[12])) * 1000000000ull / hz();
        t.priority = std::stoi(v[15]);
        t.nice = std::stoi(v[16]);
        t.threads = std::stoi(v[17]);
        t.id.start_ns = std::stoull(v[19]) * 1000000000ull / hz();
        t.virtual_bytes = std::stoull(v[20]);
        t.rss_bytes = std::stoull(v[21]) * page();
        t.memory_valid = true;
    } catch (...) {
        return false;
    }
    return true;
}
void statm(const std::string &path, Task &t) {
    std::ifstream f(path);
    uint64_t virt = 0, res = 0, shr = 0;
    if (f >> virt >> res >> shr) {
        t.virtual_bytes = virt * page();
        t.rss_bytes = res * page();
        t.shared_bytes = shr * page();
    }
}
class ProcSource final : public TaskSource {
    size_t scanned_{};

  public:
    std::string name() const override {
        return "procfs";
    }
    size_t scanned_tasks() const override {
        return scanned_;
    }
    bool collect(std::vector<Task> &out, bool threads, std::string &error) override {
        DIR *proc = opendir("/proc");
        if (!proc) {
            error = "cannot open /proc";
            return false;
        }
        out.clear();
        out.reserve(4096);
        while (auto *e = readdir(proc)) {
            if (!decimal(e->d_name))
                continue;
            int pid = atoi(e->d_name);
            std::string base = "/proc/" + std::to_string(pid);
            struct stat st {};
            if (stat(base.c_str(), &st))
                continue;
            if (!threads) {
                Task t;
                t.id.pid = t.id.tid = pid;
                t.uid = st.st_uid;
                if (!read_stat(base + "/stat", t))
                    continue;
                statm(base + "/statm", t);
                out.push_back(std::move(t));
                continue;
            }
            std::string taskdir = base + "/task";
            DIR *dir = opendir(taskdir.c_str());
            if (!dir)
                continue;
            while (auto *te = readdir(dir)) {
                if (!decimal(te->d_name))
                    continue;
                Task t;
                t.id.pid = pid;
                t.id.tid = atoi(te->d_name);
                t.uid = st.st_uid;
                if (!read_stat(taskdir + "/" + te->d_name + "/stat", t))
                    continue;
                statm(base + "/statm", t);
                out.push_back(std::move(t));
            }
            closedir(dir);
        }
        closedir(proc);
        scanned_ = out.size();
        return true;
    }
};
class HtopSource final : public TaskSource {
    size_t scanned_{};

  public:
    std::string name() const override {
        return "procfs";
    }
    size_t scanned_tasks() const override {
        return scanned_;
    }
    bool collect(std::vector<Task> &out, bool threads, std::string &error) override {
        DIR *proc = opendir("/proc");
        if (!proc) {
            error = "cannot open /proc";
            return false;
        }
        scanned_ = 0;
        out.clear();
        out.reserve(threads ? 4096 : 512);
        while (auto *e = readdir(proc)) {
            if (!decimal(e->d_name))
                continue;
            int pid = atoi(e->d_name);
            std::string base = "/proc/" + std::to_string(pid);
            struct stat st {};
            if (stat(base.c_str(), &st))
                continue;
            Task leader;
            leader.id.pid = leader.id.tid = pid;
            leader.uid = st.st_uid;
            if (!read_stat(base + "/stat", leader))
                continue;
            statm(base + "/statm", leader);
            ++scanned_;
            std::string taskdir = base + "/task";
            DIR *dir = opendir(taskdir.c_str());
            if (dir) {
                if (threads) {
                    Task leader_thread = leader;
                    if (read_stat(taskdir + "/" + std::to_string(pid) + "/stat", leader_thread)) {
                        leader_thread.virtual_bytes = leader.virtual_bytes;
                        leader_thread.rss_bytes = leader.rss_bytes;
                        leader_thread.shared_bytes = leader.shared_bytes;
                        leader = std::move(leader_thread);
                    }
                }
                while (auto *te = readdir(dir)) {
                    if (!decimal(te->d_name))
                        continue;
                    int tid = atoi(te->d_name);
                    if (tid == pid)
                        continue;
                    Task task;
                    task.id.pid = pid;
                    task.id.tid = tid;
                    task.uid = st.st_uid;
                    if (!read_stat(taskdir + "/" + te->d_name + "/stat", task))
                        continue;
                    // A thread shares its leader's address space, as in htop's statm scan.
                    task.virtual_bytes = leader.virtual_bytes;
                    task.rss_bytes = leader.rss_bytes;
                    task.shared_bytes = leader.shared_bytes;
                    task.memory_valid = leader.memory_valid;
                    ++scanned_;
                    if (threads)
                        out.push_back(std::move(task));
                }
                closedir(dir);
            }
            out.push_back(std::move(leader));
        }
        closedir(proc);
        return true;
    }
};
} // namespace
std::unique_ptr<TaskSource> make_proc_source() {
    return std::make_unique<ProcSource>();
}
std::unique_ptr<TaskSource> make_htop_source() {
    return std::make_unique<HtopSource>();
}
bool verify_identity(const Identity &id) {
    Task t;
    t.id.pid = id.pid;
    t.id.tid = id.tid;
    std::string path =
        "/proc/" + std::to_string(id.pid) + "/task/" + std::to_string(id.tid) + "/stat";
    if (!read_stat(path, t))
        return false;
    const uint64_t tick = 1000000000ull / hz();
    return t.id.start_ns <= id.start_ns && id.start_ns - t.id.start_ns < tick;
}
bool signal_task(const Identity &id, int sig, std::string &error) {
    if (!verify_identity(id)) {
        error = "task changed or exited";
        return false;
    }
    int fd = syscall(SYS_pidfd_open, id.tid, 0);
    if (fd >= 0) {
        bool ok = syscall(SYS_pidfd_send_signal, fd, sig, nullptr, 0) == 0;
        int saved = errno;
        close(fd);
        if (ok)
            return true;
        errno = saved;
    } else if (errno == ENOSYS || errno == EINVAL) {
        if (syscall(SYS_tgkill, id.pid, id.tid, sig) == 0)
            return true;
    }
    error = std::string("signal failed: ") + strerror(errno);
    return false;
}
bool renice_task(const Identity &id, int nice, std::string &error) {
    if (!verify_identity(id)) {
        error = "task changed or exited";
        return false;
    }
    if (setpriority(PRIO_PROCESS, id.tid, nice) == 0)
        return true;
    error = std::string("renice failed: ") + strerror(errno);
    return false;
}
} // namespace btitop
