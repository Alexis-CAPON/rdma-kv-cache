#pragma once

#include <cstdint>
#include <vector>
#include <thread>
#include <atomic>
#include <memory>
#include "cpp/nodes/event_queue.h"
#include "cpp/nodes/worker.h"
#include "cpp/nodes/epoll_worker.h"
#include "cpp/common/config.h"

class WorkerPool
{
public:
    // Constructor takes references to shared resources
    WorkerPool(
        uint32_t num_workers,
        EventQueue &event_queue,
        EpollWorker &epoll_worker,
        const Config &config,
        uint64_t node_id);

    ~WorkerPool();

    // Start all worker threads
    void start();

    // Stop all worker threads gracefully
    void stop();

    // Get operation counters for server-side throughput tracking
    uint64_t get_total_ops() const { return total_ops_.load(); }

private:
    // Configuration
    uint32_t num_workers_;
    uint64_t node_id_;
    std::atomic<bool> running_;

    // Server-side operation counter for throughput tracking
    std::atomic<uint64_t> total_ops_;

    // Shared resources (references to objects owned by Node)
    EventQueue &event_queue_;

    EpollWorker &epoll_worker_;
    Config config_;

    // Worker threads
    std::vector<std::thread> threads_;

    // Worker instances (one per thread)
    std::vector<std::unique_ptr<Worker>> workers_;

    // Thread function: polls queue and delegates to Worker
    void worker_thread_function(uint32_t worker_id);
};
