#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include "cpp/nodes/event_queue.h"
#include "cpp/common/config.h"
#include "cpp/common/messages.h"
#include "cpp/common/types.h"
#include "cpp/nodes/worker_base.h"
#include "cpp/apps/orchestrator/node_registry.h"
#include "cpp/apps/orchestrator/rdma_exchange_tracker.h"
#include "cpp/apps/orchestrator/request_tracker.h"
#include "cpp/apps/orchestrator/request_router.h"
#include "cpp/nodes/epoll_worker_orchestrator.h"

class WorkerOrchestrator : public WorkerBase
{
public:
    // Instance for Orchestrator
    WorkerOrchestrator(
        uint32_t worker_id,
        EpollWorkerOrchestrator &epoll_worker,
        const Config &config,
        const std::string &node_id,
        std::atomic<uint64_t> *server_ops_counter,
        NodeRegistry &node_registry,
        RdmaExchangeTracker &rdma_exchange_tracker,
        RequestTrackerOrchestrator &request_tracker_orchestrator,
        RequestRouter &request_router);

    // Main entry point: process one event
    void process_event(Event &event) override;

private:
    EpollWorkerOrchestrator &epoll_worker_;

    NodeRegistry &node_registry_;                              // Only used by orchestrator worker
    RdmaExchangeTracker &rdma_exchange_tracker_;               // Only used by orchestrator worker
    RequestTrackerOrchestrator &request_tracker_orchestrator_; // Only used by orchestrator worker
    RequestRouter &request_router_;                            // Only used by orchestrator worker

    // ========================================
    // Event Handlers
    // ========================================
    // Only orchestrator nodes
    void handle_client_request(int client_fd, Message *msg);

    // Only orchestrator nodes
    void handle_process_rdma_registration(int server_fd, Message *msg);
    void handle_ready(int server_fd, Message *msg);
    void handle_prefill_complete_orchestrator(int server_fd, Message *msg);
    void handle_decode_complete(int server_fd, Message *msg);
    void handle_request_failed(int server_fd, Message *msg);

    // ========================================
    // Response Handling
    // ========================================
    void send_client_response(int client_fd, const Message &response);
    void send_server_response(int server_fd, const Message &response);
};
