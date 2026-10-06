#pragma once
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace btitop {
using Clock = std::chrono::steady_clock;
struct Identity {
    int pid{}, tid{};
    uint64_t start_ns{};
    bool operator==(const Identity &) const = default;
};
struct IdentityHash {
    size_t operator()(const Identity &x) const noexcept;
};
struct Task {
    Identity id;
    int ppid{}, uid{}, priority{}, nice{}, threads{1};
    char state{'?'};
    uint64_t cpu_ns{}, virtual_bytes{}, rss_bytes{}, shared_bytes{};
    bool memory_valid{false};
    std::string name, command;
    std::optional<double> cpu_percent, mem_percent;
};
struct CpuTimes {
    uint64_t user{}, nice{}, system{}, idle{}, iowait{}, irq{}, softirq{}, steal{};
    uint64_t total() const;
    uint64_t busy() const;
};
struct SystemInfo {
    std::vector<CpuTimes> cpus;
    std::vector<std::optional<double>> cpu_percent;
    uint64_t memory_total{}, memory_available{}, swap_total{}, swap_free{};
    double uptime{}, load[3]{};
};
struct Snapshot {
    Clock::time_point begin{}, end{};
    uint64_t boottime_ns{};
    bool thread_mode{};
    std::string mode, backend, diagnostic;
    size_t scanned_tasks{};
    SystemInfo system;
    std::vector<Task> tasks;
};
class TaskSource {
  public:
    virtual ~TaskSource() = default;
    virtual std::string name() const = 0;
    virtual bool collect(std::vector<Task> &out, bool threads, std::string &error) = 0;
    virtual size_t scanned_tasks() const = 0;
};
std::unique_ptr<TaskSource> make_proc_source();
std::unique_ptr<TaskSource> make_htop_source();
std::unique_ptr<TaskSource> make_bpf_source(std::string &error);
SystemInfo read_system();
class Metrics {
    std::unordered_map<Identity, uint64_t, IdentityHash> last_;
    std::unordered_map<Identity, uint64_t, IdentityHash> next_;
    std::vector<CpuTimes> last_cpu_;
    Clock::time_point last_time_{};
    uint64_t last_boot_ns_{};
    std::string backend_;

  public:
    void apply(Snapshot &snapshot, bool normalize = false);
    void reset();
};
enum class Sort { CPU, MEMORY, PID, TIME, NAME };
struct Query {
    std::optional<int> pid, uid;
    std::string search;
    Sort sort{Sort::CPU};
    bool descending{true};
    bool tree{false};
};
std::vector<size_t> select(const std::vector<Task> &tasks, const Query &q);
std::string state_name(char state);
std::string format_bytes(uint64_t bytes);
std::string format_time(uint64_t ns);
std::string username(int uid);
std::string command_line(int pid);
bool verify_identity(const Identity &id);
bool signal_task(const Identity &id, int sig, std::string &error);
bool renice_task(const Identity &id, int nice, std::string &error);
} // namespace btitop
