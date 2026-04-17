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

class Worker
{
public:
    Worker(
        uint32_t worker_id,
        EpollWorker &epoll_worker,
        const Config &config,
        uint64_t node_id,
        std::atomic<uint64_t> *server_ops_counter = nullptr);

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

    // Shared server-side operation counter (for throughput tracking)
    std::atomic<uint64_t> *server_ops_counter_;

    // Per-worker statistics
    uint64_t events_processed_;

    // ========================================
    // Event Handlers
    // ========================================
    // Only orchestrator nodes
    void handle_client_request(int client_fd, Message *msg);
    void handle_client_response(int client_fd, Message *msg);
    void handle_client_error(int client_fd, Message *msg);

    // Only orchestrator nodes
    void handle_assign_request(int server_fd, Message *msg);
    void handle_process_rdma_registration(int server_fd, Message *msg);
    void handle_ready(int server_fd, Message *msg);

    // Only prefill / decode nodes
    void handle_node_rdma_registration_complete(int server_fd, Message *msg);
    void handle_request_failed(int server_fd, Message *msg);
    void handle_broadcast_member_info(Message *msg);

    // Only prefill nodes
    void handle_transfer_ready(int server_fd, Message *msg);
    void handle_prefill_complete(int server_fd, Message *msg);

    // Only decode nodes
    void handle_decode_complete(int server_fd, Message *msg);

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
