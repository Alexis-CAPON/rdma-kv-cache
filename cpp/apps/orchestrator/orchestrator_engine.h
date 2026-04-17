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
#include "cpp/apps/orchestrator/node_registry.h"
#include "cpp/apps/orchestrator/request_router.h"
#include "cpp/apps/orchestrator/request_tracker.h"
#include "cpp/apps/orchestrator/rdma_exchange_tracker.h"

/**
 * OrchestratorEngine - Main orchestrator for RDMA KV Cache cluster
 *
 * Responsibilities:
 * 1. Accept client requests and route to prefill nodes
 * 2. Coordinate RDMA QP exchange during cluster initialization
 * 3. Track request lifecycle (prefill -> decode -> response)
 * 4. Monitor node health
 *
 * Architecture:
 * - 2 TCP servers: one for clients, one for prefill/decode nodes
 * - EpollWorker: I/O thread handling all TCP connections
 * - WorkerPool: Thread pool processing events from queue
 * - Shared components: NodeRegistry, RequestRouter, RequestTracker, RdmaExchangeTracker
 */
class OrchestratorEngine
{
public:
    explicit OrchestratorEngine(Config config);
    ~OrchestratorEngine() = default;

    // Non-copyable
    OrchestratorEngine(const OrchestratorEngine &) = delete;
    OrchestratorEngine &operator=(const OrchestratorEngine &) = delete;

    // ========================================
    // Lifecycle
    // ========================================

    /**
     * Start orchestrator services
     * 1. Bind and listen on both TCP servers
     * 2. Start EpollWorker I/O thread
     * 3. Start WorkerPool threads
     * 4. Transition to WAITING_FOR_NODES state
     */
    bool start();

    /**
     * Shutdown orchestrator gracefully
     * 1. Stop accepting new requests
     * 2. Stop EpollWorker
     * 3. Stop WorkerPool (let workers finish current tasks)
     * 4. Close TCP servers
     */
    void shutdown();

    /**
     * Check if orchestrator is running
     */
    bool is_running() const { return running_; }

    // ========================================
    // State
    // ========================================

    enum class State
    {
        STARTING,          // Initial state
        WAITING_FOR_NODES, // Waiting for node registrations
        QP_EXCHANGING,     // Broadcasting QP maps to nodes
        WAITING_FOR_READY, // Waiting for RDMA_READY from all nodes
        RUNNING,           // Accepting client requests
        STOPPED
    };

    /**
     * Get current orchestrator state
     */
    State get_state() const { return state_.load(); }

    /**
     * Set orchestrator state
     */
    void set_state(State state) { state_.store(state); }

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

    // State
    std::atomic<bool> running_;
    std::atomic<State> state_;

    // TCP Servers
    TCPServer client_tcp_server_; // For client connections (port from config.client_socket_port)
    TCPServer server_tcp_server_; // For prefill/decode node connections (port from config.server_socket_port)

    // Event processing
    EventQueue event_queue_;
    EpollWorker epoll_worker_;
    WorkerPool worker_pool_;

    NodeRegistry node_registry_;
    RequestRouter request_router_;
    RequestTracker request_tracker_;
    RdmaExchangeTracker rdma_exchange_tracker_;
};
