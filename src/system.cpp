#include "btitop/core.hpp"
#include <fstream>
#include <sstream>
#include <string>
namespace btitop {
SystemInfo read_system() {
    SystemInfo s;
    std::ifstream f("/proc/stat");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("cpu", 0) != 0)
            break;
        std::istringstream in(line);
        std::string label;
        CpuTimes t;
        in >> label >> t.user >> t.nice >> t.system >> t.idle >> t.iowait >> t.irq >> t.softirq >>
            t.steal;
        if (label == "cpu" || label.size() > 3)
            s.cpus.push_back(t);
    }
    std::ifstream mem("/proc/meminfo");
    std::string key;
    uint64_t value;
    std::string unit;
    while (mem >> key >> value >> unit) {
        if (key == "MemTotal:")
            s.memory_total = value * 1024;
        else if (key == "MemAvailable:")
            s.memory_available = value * 1024;
        else if (key == "SwapTotal:")
            s.swap_total = value * 1024;
        else if (key == "SwapFree:")
            s.swap_free = value * 1024;
    }
    std::ifstream up("/proc/uptime");
    up >> s.uptime;
    std::ifstream load("/proc/loadavg");
    load >> s.load[0] >> s.load[1] >> s.load[2];
    return s;
}
} // namespace btitop
