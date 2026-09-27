#include "btitop/core.hpp"
#include <cassert>
#include <chrono>
using namespace btitop;
int main() {
    Metrics m;
    Snapshot a;
    a.backend = "test";
    a.end = Clock::now();
    a.system.memory_total = 1000;
    Task t;
    t.id = {1, 1, 100};
    t.cpu_ns = 100;
    t.rss_bytes = 100;
    t.memory_valid = true;
    t.name = "alpha";
    a.tasks.push_back(t);
    m.apply(a);
    assert(!a.tasks[0].cpu_percent);
    assert(a.tasks[0].mem_percent == 10);
    Snapshot b = a;
    b.end = a.end + std::chrono::seconds(1);
    b.tasks[0].cpu_ns += 500000000;
    m.apply(b);
    assert(b.tasks[0].cpu_percent && *b.tasks[0].cpu_percent > 49 && *b.tasks[0].cpu_percent < 51);
    b.end += std::chrono::seconds(1);
    b.tasks[0].id.start_ns++;
    m.apply(b);
    assert(!b.tasks[0].cpu_percent);
    b.backend = "other";
    m.apply(b);
    assert(!b.tasks[0].cpu_percent);
    Query q;
    q.search = "alpha";
    assert(select(b.tasks, q).size() == 1);
}
