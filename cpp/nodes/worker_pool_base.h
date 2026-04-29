#pragma once

#include <cstdint>
#include <vector>
#include <thread>
#include <atomic>
#include <memory>
#include "cpp/nodes/event_queue.h"
#include "cpp/common/config.h"
#include "cpp/common/types.h"

class WorkerPoolBase
{
public:
    // Constructor for prefill/decode nodes

    ~WorkerPoolBase();

    // Get operation counters for server-side throughput tracking
    uint64_t get_total_ops() const { return total_ops_.load(); }

protected:
    WorkerPoolBase(
        uint32_t num_workers,
        EventQueue &event_queue,
        const Config &config,
        const std::string &node_id);

    // Start all worker threads
    virtual void start() = 0;

    // Stop all worker threads gracefully
    virtual void stop() = 0;

    // Thread function: polls queue and delegates to Worker
    virtual void worker_thread_function(uint32_t worker_id) = 0;

    // Configuration
    uint32_t num_workers_;
    std::string node_id_;
    std::atomic<bool> running_;

    // Server-side operation counter for throughput tracking
    std::atomic<uint64_t> total_ops_;

    // Shared resources (references to objects owned by Node)
    EventQueue &event_queue_;
    Config config_;

    // Worker threads
    std::vector<std::thread> threads_;
};
