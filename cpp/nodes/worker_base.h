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

class WorkerBase
{
public:
    ~WorkerBase();

    // Main entry point: process one event
    virtual void process_event(Event &event) = 0;

    // Get worker statistics
    uint64_t get_events_processed() const { return events_processed_; }

protected:
    WorkerBase(
        uint32_t worker_id,
        const Config &config,
        const std::string &node_id,
        std::atomic<uint64_t> *server_ops_counter);

    // Worker identity
    uint32_t worker_id_;
    std::string node_id_;

    Config config_;

    // Shared server-side operation counter (for throughput tracking)
    std::atomic<uint64_t> *server_ops_counter_;

    // Per-worker statistics
    uint64_t events_processed_;

    // ========================================
    // Helper Functions
    // ========================================
    uint64_t generate_transaction_id();

    // ========================================
    // Response Handling
    // ========================================
    virtual void send_client_response(int client_fd, const Message &response) = 0;
    virtual void send_server_response(int server_fd, const Message &response) = 0;
};
