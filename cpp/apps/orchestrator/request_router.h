#pragma once
#include "cpp/common/types.h"
#include "cpp/apps/orchestrator/node_registry.h"
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <atomic>

/**
 * RequestRouter - Selects nodes for request assignment
 *
 * Implements different routing policies:
 * - ROUND_ROBIN: Distribute evenly across nodes
 * - LEAST_LOADED: Send to node with fewest active requests
 * - RANDOM: Random selection
 */
class RequestRouter
{
public:
    enum class RoutingPolicy
    {
        ROUND_ROBIN,
        LEAST_LOADED,
        RANDOM
    };

    explicit RequestRouter(NodeRegistry &node_registry,
                           RoutingPolicy policy = RoutingPolicy::LEAST_LOADED);
    ~RequestRouter() = default;

    // ========================================
    // Node Selection
    // ========================================

    /**
     * Select a prefill node for a new request
     * Returns nullopt if no healthy nodes available
     */
    std::optional<NodeInfo> select_prefill_node();

    /**
     * Select a decode node (general)
     * Returns nullopt if no healthy nodes available
     */
    std::optional<NodeInfo> select_decode_node();

    /**
     * Select a decode node for a specific prefill node
     * Future: Can implement affinity/locality (same rack, etc.)
     * Returns nullopt if no healthy nodes available
     */
    std::optional<NodeInfo> select_decode_node_for_prefill(
        const std::string &prefill_node_id);

    // ========================================
    // Load Tracking
    // ========================================

    /**
     * Increment active request count for a node
     */
    void increment_node_load(const std::string &node_id);

    /**
     * Decrement active request count for a node
     */
    void decrement_node_load(const std::string &node_id);

    /**
     * Get current load for a node
     */
    int get_node_load(const std::string &node_id) const;

    /**
     * Reset load for a node (e.g., on node reconnect)
     */
    void reset_node_load(const std::string &node_id);

    // ========================================
    // Configuration
    // ========================================

    /**
     * Change routing policy
     */
    void set_policy(RoutingPolicy policy);

    /**
     * Get current policy
     */
    RoutingPolicy get_policy() const;

private:
    NodeRegistry &node_registry_;
    RoutingPolicy policy_;

    // Load tracking
    mutable std::shared_mutex load_mutex_;
    std::unordered_map<std::string, int> node_loads_;

    // Round-robin state
    std::atomic<size_t> prefill_rr_index_{0};
    std::atomic<size_t> decode_rr_index_{0};

    // Helper methods
    std::optional<NodeInfo> select_round_robin(const std::vector<NodeInfo> &nodes,
                                               std::atomic<size_t> &index);
    std::optional<NodeInfo> select_least_loaded(const std::vector<NodeInfo> &nodes);
    std::optional<NodeInfo> select_random(const std::vector<NodeInfo> &nodes);
};
