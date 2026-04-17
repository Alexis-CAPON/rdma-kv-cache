#pragma once

#include "cpp/common/types.h"
#include <unordered_set>
#include <shared_mutex>
#include <condition_variable>
#include <chrono>

/**
 * RdmaExchangeTracker - Tracks RDMA initialization state during cluster startup
 *
 * Responsibilities:
 * 1. Track which nodes have registered (sent RDMA_PROCESS_REGISTRATION)
 * 2. Determine when all expected nodes have registered
 * 3. Track which nodes are RDMA ready (sent RDMA_READY after QP connection)
 * 4. Determine when cluster is fully operational
 *
 * Does NOT store node data - that's NodeRegistry's job.
 * This is purely a state tracker for the initialization process.
 */
class RdmaExchangeTracker
{
public:
    explicit RdmaExchangeTracker(int expected_prefill_nodes,
                                 int expected_decode_nodes);
    ~RdmaExchangeTracker() = default;

    /**
     * Register a node (called when RDMA_PROCESS_REGISTRATION received)
     * Returns true if this was the last expected node
     */
    bool register_node(const std::string &node_id, NodeRole role);

    /**
     * Check if all expected nodes have registered
     */
    bool all_nodes_registered() const;

    /**
     * Wait for all nodes to register (with timeout)
     * Returns true if all registered, false if timeout
     */
    bool wait_for_all_nodes(std::chrono::milliseconds timeout);

    /**
     * Get set of registered node IDs
     */
    std::unordered_set<std::string> get_registered_nodes() const;

    // ========================================
    // Ready Phase
    // ========================================

    /**
     * Mark a node as RDMA ready (QPs connected)
     * Returns true if this was the last node
     */
    bool mark_node_ready(const std::string &node_id);

    /**
     * Check if all registered nodes are RDMA ready
     */
    bool all_nodes_ready() const;

    /**
     * Wait for all nodes to be ready (with timeout)
     * Returns true if all ready, false if timeout
     */
    bool wait_for_all_ready(std::chrono::milliseconds timeout);

    /**
     * Get set of ready node IDs
     */
    std::unordered_set<std::string> get_ready_nodes() const;

    // ========================================
    // Statistics
    // ========================================

    struct Stats
    {
        int expected_prefill_nodes;
        int expected_decode_nodes;
        int registered_prefill_nodes;
        int registered_decode_nodes;
        int ready_nodes;
        bool topology_validated;
        bool cluster_operational;
    };

    Stats get_stats() const;

private:
    // Expected cluster size
    int expected_prefill_nodes_;
    int expected_decode_nodes_;

    // Registration tracking
    mutable std::shared_mutex mutex_;
    std::condition_variable_any cv_;
    std::unordered_set<std::string> registered_nodes_;
    std::unordered_set<std::string> registered_prefill_;
    std::unordered_set<std::string> registered_decode_;

    // Ready tracking
    std::unordered_set<std::string> ready_nodes_;
};
