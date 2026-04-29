#pragma once

#include <cstdint>
#include <vector>
#include <thread>
#include <atomic>
#include <memory>
#include "cpp/nodes/event_queue.h"
#include "cpp/nodes/worker_orchestrator.h"
#include "cpp/nodes/epoll_worker_orchestrator.h"
#include "cpp/common/config.h"
#include "cpp/apps/orchestrator/node_registry.h"
#include "cpp/apps/orchestrator/rdma_exchange_tracker.h"
#include "cpp/apps/orchestrator/request_tracker.h"
#include "cpp/apps/orchestrator/request_router.h"
#include "cpp/nodes/node.h"

#include "cpp/nodes/worker_pool_base.h"

#include "cpp/common/types.h"

class WorkerPoolOrchestrator : public WorkerPoolBase
{
public:
    // Constructor for orchestrator node
    WorkerPoolOrchestrator(
        uint32_t num_workers,
        EventQueue &event_queue,
        EpollWorkerOrchestrator &epoll_worker,
        const Config &config,
        const std::string &node_id,
        NodeRegistry &node_registry,
        RdmaExchangeTracker &rdma_exchange_tracker,
        RequestTrackerOrchestrator &request_tracker_orchestrator,
        RequestRouter &request_router);

    // Start all worker threads
    void start() override;

    // Stop all worker threads gracefully
    void stop() override;

private:
    NodeRegistry &node_registry_;                              // Only used by orchestrator worker
    RdmaExchangeTracker &rdma_exchange_tracker_;               // Only used by orchestrator worker
    RequestTrackerOrchestrator &request_tracker_orchestrator_; // Only used by orchestrator
    RequestRouter &request_router_;                            // Only used by orchestrator worker

    EpollWorkerOrchestrator &epoll_worker_;

    // Worker instances (one per thread)
    std::vector<std::unique_ptr<WorkerOrchestrator>> workers_;

    // Thread function: polls queue and delegates to Worker
    void worker_thread_function(uint32_t worker_id) override;
};
