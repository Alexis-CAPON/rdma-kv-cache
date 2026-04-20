#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <memory>
#include "cpp/common/config.h"
#include "cpp/common/logger.h"
#include "cpp/nodes/epoll_worker.h"
#include "cpp/nodes/worker_pool.h"
#include "cpp/nodes/event_queue.h"
#include "cpp/network/tcp_server.h"
#include "cpp/rdma/rdma_engine.h"
#include "cpp/common/types.h"
#include "cpp/nodes/request_tracker.h"

class Node
{
public:
    explicit Node(Config config);
    ~Node() = default;

    // Non-copyable
    Node(const Node &) = delete;
    Node &operator=(const Node &) = delete;

    // ========================================
    // Lifecycle
    // ========================================

    /**
     * Start node services
     * 1. Bind and listen on both TCP servers
     * 2. Start EpollWorker I/O thread
     * 3. Start WorkerPool threads
     * 4. Connect to orchestrator and send NodeInfo
     * 5. Transition to WAITING_FOR_ORCHESTRATOR_BROADCAST state
     */
    bool start();

    /**
     * Shutdown node gracefully
     * 1. Stop accepting new requests
     * 2. Stop EpollWorker
     * 3. Stop WorkerPool (let workers finish current tasks)
     * 4. Close TCP servers
     */
    void shutdown();

    /**
     * Check if node is running
     */
    bool is_running() const { return running_; }

    // ========================================
    // State
    // ========================================

    enum class State
    {
        STARTING,                           // Initial state
        WAITING_FOR_ORCHESTRATOR_BROADCAST, // Waiting for orchestrator broadcast
        RUNNING,
        STOPPED
    };

    /**
     * Get current node state
     */
    static State get_state() { return state_.load(); }

    /**
     * Set node state
     */
    static void set_state(State state) { state_.store(state); }

    // ========================================
    // Accessors (for debugging/monitoring)
    // ========================================

    uint32_t get_id() const { return config_.node_id; }
    uint32_t get_client_port() const { return config_.client_socket_port; }
    uint32_t get_server_port() const { return config_.server_socket_port; }
    std::string get_hostname() const { return config_.hostname; }

private:
    // Configuration
    Config config_;

    NodeInfo node_info_;

    // State
    std::atomic<bool> running_;
    static std::atomic<State> state_;

    TCPServer server_tcp_server_; // For prefill/decode node connections (port from config.server_socket_port)

    // Event processing
    EventQueue event_queue_;
    EpollWorker epoll_worker_;
    WorkerPool worker_pool_;

    // RDMA

    RDMAEngine rdma_engine_;
};
