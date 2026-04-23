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
#include "cpp/apps/orchestrator/node_registry.h"
#include "cpp/apps/orchestrator/rdma_exchange_tracker.h"
#include "cpp/apps/orchestrator/request_tracker.h"
#include "cpp/apps/orchestrator/request_router.h"

#include "cpp/rdma/rdma_engine.h"
#include "cpp/common/types.h"

class WorkerPool
{
public:
    // Constructor for prefill/decode nodes
    WorkerPool(
        uint32_t num_workers,
        EventQueue &event_queue,
        EpollWorker &epoll_worker,
        const Config &config,
        uint64_t node_id,
        NodeInfo &node_info,
        RDMAEngine &rdma_engine);

    // Constructor for orchestrator node
    WorkerPool(
        uint32_t num_workers,
        EventQueue &event_queue,
        EpollWorker &epoll_worker,
        const Config &config,
        uint64_t node_id,
        NodeRegistry &node_registry,
        RdmaExchangeTracker &rdma_exchange_tracker,
        RequestTrackerOrchestrator &request_tracker_orchestrator,
        RequestRouter &request_router);

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
    NodeRegistry &node_registry_;                              // Only used by orchestrator worker
    RdmaExchangeTracker &rdma_exchange_tracker_;               // Only used by orchestrator worker
    RequestTrackerOrchestrator &request_tracker_orchestrator_; // Only used by orchestrator
    RequestRouter &request_router_;                            // Only used by orchestrator worker

    NodeInfo &node_info_;     // only used by prefill/decode workers
    RDMAEngine &rdma_engine_; // only used by prefill/decode workers

    EpollWorker &epoll_worker_;
    Config config_;

    // Worker threads
    std::vector<std::thread> threads_;

    // Worker instances (one per thread)
    std::vector<std::unique_ptr<Worker>> workers_;

    // Thread function: polls queue and delegates to Worker
    void worker_thread_function(uint32_t worker_id);
};
