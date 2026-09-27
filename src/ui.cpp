#include "btitop/ui.hpp"
#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <ncurses.h>
#include <set>
#include <sstream>
#include <unordered_map>
namespace btitop {
namespace {
volatile std::sig_atomic_t interrupted = 0;
void interrupt_handler(int) {
    interrupted = 1;
}
std::string json_quote(std::string_view v) {
    std::string o = "\"";
    for (unsigned char c : v) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += c;
        } else if (c == '\n')
            o += "\\n";
        else if (c < 32) {
            char b[7];
            snprintf(b, sizeof(b), "\\u%04x", c);
            o += b;
        } else
            o += c;
    }
    return o + '"';
}
std::string pct(std::optional<double> n) {
    if (!n)
        return "N/A";
    std::ostringstream o;
    o << std::fixed << std::setprecision(1) << *n;
    return o.str();
}
std::string cells(const Task &t, bool command, bool tree, int depth,
                  const std::vector<std::string> &fields) {
    std::ostringstream o;
    for (auto &f : fields) {
        if (o.tellp() > 0)
            o << ' ';
        std::string value;
        int width = 0;
        bool right = true;
        if (f == "PID") {
            value = std::to_string(t.id.tid);
            width = 7;
        } else if (f == "USER") {
            value = username(t.uid);
            width = 8;
            right = false;
        } else if (f == "PR") {
            value = std::to_string(t.priority);
            width = 3;
        } else if (f == "NI") {
            value = std::to_string(t.nice);
            width = 3;
        } else if (f == "VIRT") {
            value = format_bytes(t.virtual_bytes);
            width = 6;
        } else if (f == "RES") {
            value = t.memory_valid ? format_bytes(t.rss_bytes) : "N/A";
            width = 6;
        } else if (f == "SHR") {
            value = t.memory_valid ? format_bytes(t.shared_bytes) : "N/A";
            width = 6;
        } else if (f == "S") {
            value = t.state;
            width = 1;
        } else if (f == "%CPU") {
            value = pct(t.cpu_percent);
            width = 5;
        } else if (f == "%MEM") {
            value = pct(t.mem_percent);
            width = 5;
        } else if (f == "TIME+") {
            value = format_time(t.cpu_ns);
            width = 9;
        } else if (f == "COMMAND") {
            value = (command && !t.command.empty() ? t.command : t.name);
            if (tree)
                value = std::string(std::min(depth, 20) * 2, ' ') + value;
            right = false;
        } else
            continue;
        if (width) {
            value = value.substr(0, width);
            o << (right ? std::right : std::left) << std::setw(width) << value;
        } else
            o << value;
    }
    return o.str();
}
std::string headings(const std::vector<std::string> &fields) {
    Task empty;
    empty.id.tid = 0;
    std::string out;
    for (auto &f : fields) {
        if (!out.empty())
            out += ' ';
        int w = 0;
        if (f == "PID")
            w = 7;
        else if (f == "USER")
            w = 8;
        else if (f == "PR" || f == "NI")
            w = 3;
        else if (f == "VIRT" || f == "RES" || f == "SHR")
            w = 6;
        else if (f == "S")
            w = 1;
        else if (f == "%CPU" || f == "%MEM")
            w = 5;
        else if (f == "TIME+")
            w = 9;
        if (w)
            out += std::string(std::max(0, w - static_cast<int>(f.size())), ' ');
        out += f;
    }
    return out;
}
std::vector<std::string> parse_fields(const std::string &input) {
    static const std::set<std::string> valid = {"PID", "USER", "PR",   "NI",   "VIRT",  "RES",
                                                "SHR", "S",    "%CPU", "%MEM", "TIME+", "COMMAND"};
    std::vector<std::string> out;
    std::istringstream in(input);
    std::string part;
    while (std::getline(in, part, ',')) {
        if (!valid.contains(part) || std::find(out.begin(), out.end(), part) != out.end())
            return {};
        out.push_back(part);
    }
    return out;
}
std::string config_path() {
    const char *x = getenv("XDG_CONFIG_HOME");
    const char *h = getenv("HOME");
    if (x && *x)
        return std::string(x) + "/btitop/config";
    return h ? std::string(h) + "/.config/btitop/config" : "";
}
void load_settings(UiOptions &opt) {
    auto path = config_path();
    if (path.empty())
        return;
    std::ifstream in(path);
    std::string key, value;
    while (in >> key >> value) {
        try {
            if (key == "interval")
                opt.interval = std::clamp(std::stod(value), 0.1, 3600.0);
            else if (key == "per_core")
                opt.per_core = (value == "1");
            else if (key == "command")
                opt.command = (value == "1");
            else if (key == "fields") {
                auto f = parse_fields(value);
                if (!f.empty())
                    opt.fields = std::move(f);
            }
        } catch (...) {
        }
    }
}
void save_settings(const UiOptions &opt) {
    auto path = config_path();
    if (path.empty())
        return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    if (ec)
        return;
    std::ofstream out(path);
    if (!out)
        return;
    out << "interval " << opt.interval << "\nper_core " << opt.per_core << "\ncommand "
        << opt.command << "\nfields ";
    for (size_t i = 0; i < opt.fields.size(); i++)
        out << (i ? "," : "") << opt.fields[i];
    out << '\n';
}
void put(int y, int x, const std::string &str, int width) {
    if (y < 0 || x < 0 || y >= LINES || x >= COLS)
        return;
    mvaddnstr(y, x, str.c_str(), std::max(0, std::min(width, COLS - x)));
}
std::string prompt(const std::string &label) {
    echo();
    curs_set(1);
    nodelay(stdscr, FALSE);
    char buf[128]{};
    move(LINES - 1, 0);
    clrtoeol();
    put(LINES - 1, 0, label, COLS);
    getnstr(buf, sizeof(buf) - 1);
    noecho();
    curs_set(0);
    nodelay(stdscr, TRUE);
    return buf;
}
void draw(const Snapshot &s, UiOptions &opt, const std::vector<size_t> &ids, size_t cursor,
          const std::string &status,
          const std::unordered_map<Identity, std::string, IdentityHash> &commands) {
    erase();
    int y = 0;
    std::ostringstream title;
    title << "btitop  " << s.backend << "  up " << static_cast<int>(s.system.uptime / 3600)
          << "h  load " << std::fixed << std::setprecision(2) << s.system.load[0] << ' '
          << s.system.load[1] << ' ' << s.system.load[2] << "  tasks " << s.tasks.size();
    attron(A_BOLD);
    put(y++, 0, title.str(), COLS);
    attroff(A_BOLD);
    std::ostringstream counts;
    int running = 0, sleeping = 0, other = 0;
    for (auto &t : s.tasks) {
        if (t.state == 'R')
            running++;
        else if (t.state == 'S' || t.state == 'I')
            sleeping++;
        else
            other++;
    }
    counts << "Tasks " << s.tasks.size() << "  running " << running << "  sleeping " << sleeping
           << "  other " << other;
    put(y++, 0, counts.str(), COLS);
    std::ostringstream cpu;
    cpu << "CPU " << (s.system.cpu_percent.empty() ? "N/A" : pct(s.system.cpu_percent[0])) << "%";
    if (opt.per_core)
        for (size_t i = 1; i < s.system.cpu_percent.size() && COLS > 13 &&
                           cpu.str().size() < static_cast<size_t>(COLS - 13);
             i++)
            cpu << "  " << i - 1 << ':' << pct(s.system.cpu_percent[i]) << '%';
    put(y++, 0, cpu.str(), COLS);
    std::ostringstream mem;
    mem << "Mem " << format_bytes(s.system.memory_total - s.system.memory_available) << '/'
        << format_bytes(s.system.memory_total) << "  Swap "
        << format_bytes(s.system.swap_total - s.system.swap_free) << '/'
        << format_bytes(s.system.swap_total);
    put(y++, 0, mem.str(), COLS);
    std::string head = headings(opt.fields);
    attron(A_REVERSE);
    put(y++, 0, head, COLS);
    attroff(A_REVERSE);
    int rows = std::max(0, LINES - y - 2);
    size_t start = cursor >= static_cast<size_t>(rows) ? cursor - rows + 1 : 0;
    std::unordered_map<int, int> depth;
    if (opt.query.tree)
        for (auto id : ids) {
            const auto &t = s.tasks[id];
            depth[t.id.tid] = depth.contains(t.ppid) ? depth[t.ppid] + 1 : 0;
        }
    for (int k = 0; k < rows && start + k < ids.size(); k++) {
        const auto &t = s.tasks[ids[start + k]];
        Task copy = t;
        if (auto it = commands.find(t.id); it != commands.end())
            copy.command = it->second;
        if (start + k == cursor)
            attron(A_REVERSE);
        if (opt.normalize && copy.cpu_percent && s.system.cpus.size() > 1)
            *copy.cpu_percent /= s.system.cpus.size() - 1;
        put(y + k, 0, cells(copy, opt.command, opt.query.tree, depth[t.id.tid], opt.fields), COLS);
        if (start + k == cursor)
            attroff(A_REVERSE);
    }
    put(LINES - 2, 0,
        "q quit  h help  t threads  c command  / search  s sort  f fields  k signal  r renice",
        COLS);
    put(LINES - 1, 0, status.empty() ? s.diagnostic : status, COLS);
    refresh();
}
} // namespace
void load_ui_settings(UiOptions &options) {
    load_settings(options);
}
void print_text(const Snapshot &s, const Query &q) {
    std::cout << "btitop " << s.backend << "  load " << s.system.load[0] << ' ' << s.system.load[1]
              << ' ' << s.system.load[2] << "  tasks " << s.tasks.size() << '\n';
    std::cout << "    PID USER      PR  NI   VIRT    RES    SHR S  %CPU  %MEM     TIME+ COMMAND\n";
    for (auto i : select(s.tasks, q))
        std::cout << cells(s.tasks[i], false, false, 0, UiOptions{}.fields) << '\n';
}
void print_json(const Snapshot &s, const Query &q) {
    auto ns = [](Clock::time_point t) {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
    };
    std::cout << "{\"version\":1,\"backend\":" << json_quote(s.backend)
              << ",\"begin_monotonic_ns\":" << ns(s.begin) << ",\"end_monotonic_ns\":" << ns(s.end)
              << ",\"diagnostic\":" << json_quote(s.diagnostic)
              << ",\"system\":{\"memory_total_bytes\":" << s.system.memory_total
              << ",\"memory_available_bytes\":" << s.system.memory_available
              << ",\"swap_total_bytes\":" << s.system.swap_total
              << ",\"swap_free_bytes\":" << s.system.swap_free
              << ",\"uptime_seconds\":" << s.system.uptime << ",\"load\":[" << s.system.load[0]
              << ',' << s.system.load[1] << ',' << s.system.load[2] << "],\"cpu_percent\":";
    if (!s.system.cpu_percent.empty() && s.system.cpu_percent[0])
        std::cout << *s.system.cpu_percent[0];
    else
        std::cout << "null";
    std::cout << ",\"per_core_cpu_percent\":[";
    for (size_t i = 1; i < s.system.cpu_percent.size(); ++i) {
        if (i > 1)
            std::cout << ',';
        if (s.system.cpu_percent[i])
            std::cout << *s.system.cpu_percent[i];
        else
            std::cout << "null";
    }
    std::cout << "]},\"tasks\":[";
    bool first = true;
    for (auto i : select(s.tasks, q)) {
        const auto &t = s.tasks[i];
        if (!first)
            std::cout << ',';
        first = false;
        std::cout << "{\"pid\":" << t.id.pid << ",\"tid\":" << t.id.tid
                  << ",\"start_ns\":" << t.id.start_ns << ",\"ppid\":" << t.ppid
                  << ",\"uid\":" << t.uid << ",\"priority\":" << t.priority
                  << ",\"nice\":" << t.nice << ",\"threads\":" << t.threads
                  << ",\"name\":" << json_quote(t.name)
                  << ",\"state\":" << json_quote(std::string(1, t.state))
                  << ",\"cpu_total_ns\":" << t.cpu_ns << ",\"cpu_percent\":";
        if (t.cpu_percent)
            std::cout << *t.cpu_percent;
        else
            std::cout << "null";
        std::cout << ",\"virtual_bytes\":" << t.virtual_bytes << ",\"rss_bytes\":";
        if (t.memory_valid)
            std::cout << t.rss_bytes;
        else
            std::cout << "null";
        std::cout << ",\"shared_bytes\":";
        if (t.memory_valid)
            std::cout << t.shared_bytes;
        else
            std::cout << "null";
        std::cout << ",\"memory_percent\":";
        if (t.mem_percent)
            std::cout << *t.mem_percent;
        else
            std::cout << "null";
        std::cout << '}';
    }
    std::cout << "]}\n";
}
void run_ui(UiOptions opt, std::function<std::optional<Snapshot>()> latest,
            std::function<void(bool)> set_threads, std::function<void(double)> set_interval) {
    auto previous = std::signal(SIGINT, interrupt_handler);
    auto previous_term = std::signal(SIGTERM, interrupt_handler);
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);
    if (opt.color && has_colors()) {
        start_color();
        use_default_colors();
    }
    Snapshot s;
    bool have = false, help = false, dirty = true;
    size_t cursor = 0;
    std::string status;
    std::unordered_map<Identity, std::string, IdentityHash> commands;
    try {
        while (!interrupted) {
            if (auto n = latest()) {
                s = std::move(*n);
                have = true;
                dirty = true;
                status = s.diagnostic;
                if (commands.size() > 512)
                    commands.clear();
            }
            if (dirty && have) {
                auto ids = select(s.tasks, opt.query);
                if (cursor >= ids.size())
                    cursor = ids.empty() ? 0 : ids.size() - 1;
                if (opt.command) {
                    int rows = std::max(0, LINES - 7);
                    size_t start = cursor >= static_cast<size_t>(rows) ? cursor - rows + 1 : 0;
                    for (size_t i = start; i < ids.size() && i < start + rows; i++) {
                        int pid = s.tasks[ids[i]].id.pid;
                        const auto &id = s.tasks[ids[i]].id;
                        if (!commands.contains(id))
                            commands[id] = command_line(pid);
                    }
                }
                draw(s, opt, ids, cursor, status, commands);
                if (help) {
                    clear();
                    put(0, 0, "btitop help", COLS);
                    put(2, 0,
                        "q quit | arrows navigate | t threads | c command line | / search | u user "
                        "| p "
                        "PID",
                        COLS);
                    put(3, 0,
                        "s sort | R reverse | z tree | I CPU normalize | 1 per-core CPU | +/- "
                        "interval",
                        COLS);
                    put(4, 0, "k send signal | r renice | Enter details | h close help", COLS);
                    refresh();
                }
            } else if (dirty) {
                put(0, 0, "btitop: collecting initial snapshot...", COLS);
                refresh();
            }
            dirty = false;
            int c = getch();
            if (c == ERR) {
                napms(75);
                continue;
            }
            if (c == 'q')
                break;
            dirty = true;
            if (c == 'h') {
                help = !help;
                continue;
            }
            if (help)
                continue;
            if (c == KEY_DOWN || c == 'j') {
                cursor++;
                continue;
            }
            if (c == KEY_UP || c == 'K') {
                if (cursor)
                    cursor--;
                continue;
            }
            if (c == 'f') {
                auto f = parse_fields(prompt("Fields comma-separated: "));
                if (!f.empty())
                    opt.fields = std::move(f);
                else
                    status = "invalid fields";
            } else if (c == 't') {
                opt.threads = !opt.threads;
                set_threads(opt.threads);
                cursor = 0;
                status = opt.threads ? "thread mode" : "process mode";
            } else if (c == 'c') {
                opt.command = !opt.command;
                commands.clear();
            } else if (c == '1')
                opt.per_core = !opt.per_core;
            else if (c == 'I')
                opt.normalize = !opt.normalize;
            else if (c == 'z')
                opt.query.tree = !opt.query.tree;
            else if (c == 'R')
                opt.query.descending = !opt.query.descending;
            else if (c == 's') {
                std::string v = prompt("Sort cpu/mem/pid/time/name: ");
                if (v == "cpu")
                    opt.query.sort = Sort::CPU;
                else if (v == "mem")
                    opt.query.sort = Sort::MEMORY;
                else if (v == "pid")
                    opt.query.sort = Sort::PID;
                else if (v == "time")
                    opt.query.sort = Sort::TIME;
                else if (v == "name")
                    opt.query.sort = Sort::NAME;
            } else if (c == '/') {
                opt.query.search = prompt("Search (empty clears): ");
                cursor = 0;
            } else if (c == 'u') {
                auto v = prompt("UID (empty clears): ");
                try {
                    opt.query.uid = v.empty() ? std::nullopt : std::optional<int>(std::stoi(v));
                } catch (...) {
                    status = "invalid UID";
                }
                cursor = 0;
            } else if (c == 'p') {
                auto v = prompt("PID (empty clears): ");
                try {
                    opt.query.pid = v.empty() ? std::nullopt : std::optional<int>(std::stoi(v));
                } catch (...) {
                    status = "invalid PID";
                }
                cursor = 0;
            } else if (c == '+') {
                opt.interval = std::min(3600.0, opt.interval * 1.25);
                set_interval(opt.interval);
            } else if (c == '-') {
                opt.interval = std::max(0.1, opt.interval / 1.25);
                set_interval(opt.interval);
            } else if (have && (c == '\n' || c == KEY_ENTER || c == 'k' || c == 'r')) {
                auto ids = select(s.tasks, opt.query);
                if (ids.empty() || cursor >= ids.size())
                    continue;
                const auto &t = s.tasks[ids[cursor]];
                if (c == '\n' || c == KEY_ENTER) {
                    status = "PID " + std::to_string(t.id.pid) + " TID " +
                             std::to_string(t.id.tid) + " PPID " + std::to_string(t.ppid) + " " +
                             state_name(t.state) + " threads " + std::to_string(t.threads);
                    continue;
                }
                if (c == 'k') {
                    auto v = prompt("Signal number (empty cancels): ");
                    if (v.empty())
                        continue;
                    try {
                        int sig = std::stoi(v);
                        if (sig < 1 || sig >= NSIG) {
                            status = "invalid signal";
                            continue;
                        }
                        if (prompt("Type YES to signal PID " + std::to_string(t.id.tid) + ": ") !=
                            "YES")
                            continue;
                        std::string e;
                        if (!signal_task(t.id, sig, e))
                            status = e;
                        else
                            status = "signal sent";
                    } catch (...) {
                        status = "invalid signal";
                    }
                }
                if (c == 'r') {
                    auto v = prompt("Nice -20..19 (empty cancels): ");
                    if (v.empty())
                        continue;
                    try {
                        int nice = std::stoi(v);
                        if (nice < -20 || nice > 19) {
                            status = "invalid nice";
                            continue;
                        }
                        if (prompt("Type YES to renice PID " + std::to_string(t.id.tid) + ": ") !=
                            "YES")
                            continue;
                        std::string e;
                        if (!renice_task(t.id, nice, e))
                            status = e;
                        else
                            status = "priority changed";
                    } catch (...) {
                        status = "invalid nice";
                    }
                }
            }
        }
    } catch (...) {
        endwin();
        std::signal(SIGINT, previous);
        std::signal(SIGTERM, previous_term);
        throw;
    }
    endwin();
    save_settings(opt);
    std::signal(SIGINT, previous);
    std::signal(SIGTERM, previous_term);
}
} // namespace btitop
