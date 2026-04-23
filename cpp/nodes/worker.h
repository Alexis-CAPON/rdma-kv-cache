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
#include "cpp/nodes/epoll_worker.h"
#include "cpp/apps/orchestrator/node_registry.h"
#include "cpp/apps/orchestrator/rdma_exchange_tracker.h"
#include "cpp/rdma/rdma_engine.h"
#include "cpp/apps/orchestrator/request_tracker.h"
#include "cpp/apps/orchestrator/request_router.h"
#include "cpp/nodes/node.h"

class Worker
{
public:
    // Instance for prefill/decode nodes
    Worker(
        uint32_t worker_id,
        EpollWorker &epoll_worker,
        const Config &config,
        uint64_t node_id,
        std::atomic<uint64_t> *server_ops_counter = nullptr,
        NodeInfo &node_info,
        RDMAEngine &rdma_engine,
        Node *node_ptr = nullptr);

    // Instance for Orchestrator
    Worker(
        uint32_t worker_id,
        EpollWorker &epoll_worker,
        const Config &config,
        uint64_t node_id,
        std::atomic<uint64_t> *server_ops_counter = nullptr,
        NodeRegistry &node_registry,
        RdmaExchangeTracker &rdma_exchange_tracker,
        RequestTrackerOrchestrator &request_tracker_orchestrator,
        RequestRouter &request_router);

    ~Worker();

    // Main entry point: process one event
    void process_event(Event &event);

    // Get worker statistics
    uint64_t get_events_processed() const { return events_processed_; }

private:
    // Worker identity
    uint32_t worker_id_;
    uint64_t node_id_;

    EpollWorker &epoll_worker_;
    Config config_;

    NodeInfo &node_info_;     // only used by prefill/decode workers
    RDMAEngine &rdma_engine_; // only used by prefill/decode workers

    NodeRegistry &node_registry_;                             // Only used by orchestrator worker
    RdmaExchangeTracker rdma_exchange_tracker_;               // Only used by orchestrator worker
    RequestTrackerOrchestrator request_tracker_orchestrator_; // Only used by orchestrator worker
    RequestRouter request_router_;                            // Only used by orchestrator worker

    Node *node_ptr_; // Only used by prefill/decode workers (for vLLM server management)

    // Shared server-side operation counter (for throughput tracking)
    std::atomic<uint64_t> *server_ops_counter_;

    // Per-worker statistics
    uint64_t events_processed_;

    // ========================================
    // Event Handlers
    // ========================================
    // Only orchestrator nodes
    void handle_client_request(int client_fd, Message *msg);

    // Only orchestrator nodes
    void handle_assign_request(int server_fd, Message *msg);
    void handle_process_rdma_registration(int server_fd, Message *msg);
    void handle_ready(int server_fd, Message *msg);
    void handle_prefill_complete_orchestrator(int server_fd, Message *msg);
    void handle_decode_complete(int server_fd, Message *msg);

    // Only prefill / decode nodes
    void handle_request_failed(int server_fd, Message *msg);
    void handle_broadcast_member_info(Message *msg);

    // Only prefill nodes
    void handle_transfer_ready(int server_fd, Message *msg);

    // Only decode nodes
    void handle_prefill_complete(int server_fd, Message *msg);

    // ========================================
    // Response Handling
    // ========================================
    void send_client_response(int client_fd, const Message &response);
    void send_server_response(int server_fd, const Message &response);

    // ========================================
    // Helper Functions
    // ========================================
    uint64_t generate_transaction_id();
};
