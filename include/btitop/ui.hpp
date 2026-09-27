#pragma once
#include "btitop/core.hpp"
#include <functional>
namespace btitop {
struct UiOptions {
    Query query;
    bool threads{}, normalize{}, color{true}, command{}, per_core{};
    double interval{1.0};
    std::vector<std::string> fields{"PID", "USER", "PR",   "NI",   "VIRT",  "RES",
                                    "SHR", "S",    "%CPU", "%MEM", "TIME+", "COMMAND"};
};
void load_ui_settings(UiOptions &options);
void run_ui(UiOptions options, std::function<std::optional<Snapshot>()> latest,
            std::function<void(bool)> set_threads, std::function<void(double)> set_interval);
void print_text(const Snapshot &s, const Query &q);
void print_json(const Snapshot &s, const Query &q);
} // namespace btitop
