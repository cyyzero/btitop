#include "btitop/core.hpp"
#include "btitop/ui.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <optional>
#include <pwd.h>
#include <thread>
#include <time.h>
#include <unistd.h>
using namespace btitop;
namespace {
Sort parse_sort(const std::string &s) {
    if (s == "cpu")
        return Sort::CPU;
    if (s == "mem" || s == "memory")
        return Sort::MEMORY;
    if (s == "pid")
        return Sort::PID;
    if (s == "time")
        return Sort::TIME;
    if (s == "name")
        return Sort::NAME;
    throw std::runtime_error("invalid sort: " + s);
}
std::string value(int &i, int argc, char **argv, const std::string &arg) {
    auto p = arg.find('=');
    if (p != std::string::npos)
        return arg.substr(p + 1);
    if (i + 1 >= argc)
        throw std::runtime_error("missing value for " + arg);
    return argv[++i];
}
class Sampler {
    std::unique_ptr<TaskSource> source_;
    bool automatic_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::optional<Snapshot> latest_;
    std::string error_;
    Metrics metrics_;
    std::jthread worker_;
    bool threads_;
    double interval_;
    uint64_t epoch_{0};

  public:
    Sampler(std::unique_ptr<TaskSource> source, bool automatic, bool threads, double interval)
        : source_(std::move(source)), automatic_(automatic), threads_(threads),
          interval_(interval) {
    }
    void start() {
        worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
    }
    void run(std::stop_token stop) {
        std::stop_callback on_stop(stop, [this] { cv_.notify_all(); });
        std::optional<bool> previous_mode;
        while (!stop.stop_requested()) {
            auto begin = Clock::now();
            Snapshot s;
            s.begin = begin;
            s.backend = source_->name();
            s.system = read_system();
            std::string e;
            bool thread_mode;
            double interval;
            {
                std::scoped_lock lock(mu_);
                thread_mode = threads_;
                interval = interval_;
            }
            if (previous_mode && *previous_mode != thread_mode)
                metrics_.reset();
            previous_mode = thread_mode;
            s.thread_mode = thread_mode;
            if (!source_->collect(s.tasks, thread_mode, e)) {
                if (automatic_ && source_->name() == "bpf") {
                    source_ = make_proc_source();
                    s.backend = source_->name();
                    s.diagnostic = "BPF unavailable: " + e + "; using procfs";
                    metrics_.reset();
                    e.clear();
                    if (!source_->collect(s.tasks, thread_mode, e)) {
                        std::scoped_lock lock(mu_);
                        error_ = e;
                        cv_.notify_all();
                        return;
                    }
                } else {
                    std::scoped_lock lock(mu_);
                    error_ = e;
                    cv_.notify_all();
                    return;
                }
            }
            s.scanned_tasks = source_->scanned_tasks();
            s.end = Clock::now();
            timespec boot{};
            clock_gettime(CLOCK_BOOTTIME, &boot);
            s.boottime_ns = static_cast<uint64_t>(boot.tv_sec) * 1000000000ull + boot.tv_nsec;
            metrics_.apply(s);
            {
                std::scoped_lock lock(mu_);
                latest_ = std::move(s);
                cv_.notify_all();
            }
            std::unique_lock lock(mu_);
            auto epoch = epoch_;
            cv_.wait_until(lock,
                           begin + std::chrono::duration_cast<Clock::duration>(
                                       std::chrono::duration<double>(interval)),
                           [&] { return stop.stop_requested() || epoch_ != epoch; });
        }
    }
    std::optional<Snapshot> take() {
        std::scoped_lock lock(mu_);
        if (!error_.empty())
            throw std::runtime_error(error_);
        auto s = std::move(latest_);
        latest_.reset();
        return s;
    }
    std::optional<Snapshot> wait() {
        std::unique_lock lock(mu_);
        cv_.wait(lock, [&] { return latest_.has_value() || !error_.empty(); });
        if (!error_.empty())
            throw std::runtime_error(error_);
        auto s = std::move(latest_);
        latest_.reset();
        return s;
    }
    void set_threads(bool b) {
        std::scoped_lock lock(mu_);
        threads_ = b;
        ++epoch_;
        cv_.notify_all();
    }
    void set_interval(double n) {
        std::scoped_lock lock(mu_);
        interval_ = n;
        ++epoch_;
        cv_.notify_all();
    }
};
} // namespace
int main(int argc, char **argv) {
    try {
        std::string backend = "auto";
        UiOptions ui;
        load_ui_settings(ui);
        bool batch = false, json = false;
        int iterations = 0;
        for (int i = 1; i < argc; i++) {
            std::string a = argv[i];
            if (a == "--help" || a == "-h") {
                std::cout
                    << "btitop [--backend auto|bpf|procfs|htop] [--interval seconds] [--pid PID] "
                       "[--user USER] [--threads] [--sort cpu|mem|pid|time|name] "
                       "[--batch|--json] [--iterations N] [--no-color]\n";
                return 0;
            } else if (a.rfind("--backend", 0) == 0)
                backend = value(i, argc, argv, a);
            else if (a.rfind("--interval", 0) == 0)
                ui.interval = std::stod(value(i, argc, argv, a));
            else if (a.rfind("--pid", 0) == 0)
                ui.query.pid = std::stoi(value(i, argc, argv, a));
            else if (a.rfind("--user", 0) == 0) {
                auto v = value(i, argc, argv, a);
                auto *p = getpwnam(v.c_str());
                ui.query.uid = p ? p->pw_uid : std::stoi(v);
            } else if (a == "--threads")
                ui.threads = true;
            else if (a.rfind("--sort", 0) == 0)
                ui.query.sort = parse_sort(value(i, argc, argv, a));
            else if (a == "--batch")
                batch = true;
            else if (a == "--json") {
                json = true;
                batch = true;
            } else if (a.rfind("--iterations", 0) == 0)
                iterations = std::stoi(value(i, argc, argv, a));
            else if (a == "--no-color")
                ui.color = false;
            else
                throw std::runtime_error("unknown option: " + a);
        }
        if (ui.interval < 0.1 || ui.interval > 3600)
            throw std::runtime_error("interval must be 0.1..3600 seconds");
        if (backend != "auto" && backend != "bpf" && backend != "procfs" && backend != "htop")
            throw std::runtime_error("invalid backend");
        if (!batch && !isatty(STDIN_FILENO))
            throw std::runtime_error("interactive mode requires a terminal; use --batch");
        std::string warning;
        std::unique_ptr<TaskSource> src;
        if (backend == "htop")
            src = make_htop_source();
        else if (backend != "procfs")
            src = make_bpf_source(warning);
        if (!src) {
            if (backend == "bpf")
                throw std::runtime_error(warning);
            src = make_proc_source();
            if (backend == "auto")
                std::cerr << "btitop: BPF unavailable (" << warning << "), using procfs\n";
        }
        Sampler sampler(std::move(src), backend == "auto", ui.threads, ui.interval);
        sampler.start();
        if (batch) {
            int count = 0;
            while (iterations == 0 || count < iterations) {
                auto s = sampler.wait();
                if (s) {
                    if (json)
                        print_json(*s, ui.query);
                    else
                        print_text(*s, ui.query);
                    std::cout.flush();
                    ++count;
                }
            }
        } else
            run_ui(
                ui, [&] { return sampler.take(); }, [&](bool b) { sampler.set_threads(b); },
                [&](double n) { sampler.set_interval(n); });
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "btitop: " << e.what() << '\n';
        return 1;
    }
}
