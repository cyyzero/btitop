#pragma once
#ifdef __BPF__
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef int int32_t;
#else
#include <stdint.h>
#endif
#define BTITOP_WIRE_VERSION 1
#define BTITOP_FLAG_MM 1u
#define BTITOP_FLAG_LEADER 2u
struct btitop_wire_task {
    uint16_t version;
    uint16_t length;
    uint32_t flags;
    uint32_t pid;
    uint32_t tid;
    uint32_t ppid;
    uint32_t uid;
    uint32_t state;
    int32_t priority;
    int32_t nice;
    uint32_t threads;
    uint64_t start_ns;
    uint64_t cpu_ns;
    uint64_t dead_cpu_ns;
    uint64_t virt_bytes;
    uint64_t rss_pages;
    uint64_t shared_pages;
    char comm[16];
} __attribute__((packed));
