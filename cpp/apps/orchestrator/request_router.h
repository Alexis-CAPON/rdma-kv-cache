#pragma once
#include "cpp/common/types.h"
#include "cpp/apps/orchestrator/node_registry.h"
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <queue>
#include <atomic>

/**
 * RequestRouter - Selects nodes and allocates memory slots
 *
 * Implements different routing policies and manages per-decode-node
 * memory slots for RDMA KV cache transfer.
 *
 * Each decode node has a fixed number of slots (e.g., 8) representing
 * regions in its GPU buffer. Router allocates slots when routing
 * requests and frees them when requests complete.
 *
 * Responsibilities:
 * - ROUND_ROBIN / LEAST_LOADED / RANDOM node selection
 * - Per-decode-node slot allocation (prevents offset collisions)
 * - Load tracking per node
 * - Slot availability tracking
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

    // Configuration for memory slot management
    struct SlotConfig
    {
        int max_slots_per_node = 8;      // Max concurrent requests per decode node
        size_t buffer_size_mb = 16384;    // 16GB per decode node
        int num_layers = 32;              // Transformer layers
        size_t layer_size_mb = 128;       // 128MB per layer

        size_t get_request_size_mb() const {
            return num_layers * layer_size_mb;  // 4GB per request slot
        }

        size_t get_buffer_size_bytes() const {
            return buffer_size_mb * 1024UL * 1024UL;
        }

        size_t get_layer_size_bytes() const {
            return layer_size_mb * 1024UL * 1024UL;
        }
    };

    explicit RequestRouter(NodeRegistry &node_registry,
                           RoutingPolicy policy = RoutingPolicy::LEAST_LOADED,
                           SlotConfig slot_config = SlotConfig{});
    ~RequestRouter() = default;

    // ========================================
    // Node Selection + Slot Allocation
    // ========================================

    /**
     * Routing result with slot allocation
     */
    struct RoutingResult
    {
        NodeInfo prefill_node;
        NodeInfo decode_node;
        int slot_id;               // Allocated slot on decode node (0-7)
        size_t slot_base_offset;   // Precomputed base offset for this slot (bytes)

        bool is_valid() const { return slot_id >= 0; }
    };

    /**
     * Select nodes and allocate slot for request (PRIMARY METHOD)
     *
     * This is the main entry point used by orchestrator to:
     * 1. Select prefill node
     * 2. Select decode node with available slot
     * 3. Allocate slot on decode node
     * 4. Calculate base offset
     *
     * Returns nullopt if:
     * - No healthy nodes available
     * - All decode nodes are at capacity (no free slots)
     */
    std::optional<RoutingResult> select_nodes_and_allocate_slot(
        const std::string& request_id);

    /**
     * Free slot when request completes
     * MUST be called when request finishes to avoid slot leak
     */
    void free_slot(const std::string& request_id);

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

    /**
     * Get number of available slots on decode node
     */
    int get_available_slots(const std::string& decode_node_id) const;

    /**
     * Get number of used slots on decode node
     */
    int get_used_slots(const std::string& decode_node_id) const;

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

    /**
     * Get slot configuration
     */
    const SlotConfig& get_slot_config() const { return slot_config_; }

    // ========================================
    // Legacy Methods (kept for compatibility)
    // ========================================

    std::optional<NodeInfo> select_prefill_node();
    std::optional<NodeInfo> select_decode_node();
    std::optional<NodeInfo> select_decode_node_for_prefill(
        const std::string &prefill_node_id);

private:
    NodeRegistry &node_registry_;
    RoutingPolicy policy_;
    SlotConfig slot_config_;

    // Load tracking
    mutable std::shared_mutex load_mutex_;
    std::unordered_map<std::string, int> node_loads_;

    // Round-robin state
    std::atomic<size_t> prefill_rr_index_{0};
    std::atomic<size_t> decode_rr_index_{0};

    // ========================================
    // Slot Management
    // ========================================

    // Per-decode-node slot allocator
    struct NodeSlotAllocator
    {
        std::string node_id;
        int max_slots;
        std::queue<int> free_slots;                            // Available slot IDs
        std::unordered_map<std::string, int> request_to_slot;  // request_id → slot_id
        std::unordered_map<int, std::string> slot_to_request;  // slot_id → request_id

        explicit NodeSlotAllocator(const std::string& id, int max)
            : node_id(id), max_slots(max) {
            for (int i = 0; i < max_slots; i++) {
                free_slots.push(i);
            }
        }

        bool has_free_slot() const {
            return !free_slots.empty();
        }

        int available_slots() const {
            return static_cast<int>(free_slots.size());
        }

        int used_slots() const {
            return max_slots - static_cast<int>(free_slots.size());
        }
    };

    mutable std::shared_mutex slot_mutex_;
    std::unordered_map<std::string, NodeSlotAllocator> decode_node_slots_;
    std::unordered_map<std::string, std::string> request_to_decode_node_;  // For slot freeing

    // Helper methods
    void initialize_node_slots(const std::string& decode_node_id);
    std::optional<int> allocate_slot_for_node(
        const std::string& decode_node_id,
        const std::string& request_id);
    bool free_slot_for_node(
        const std::string& decode_node_id,
        const std::string& request_id);

    std::optional<NodeInfo> select_round_robin(const std::vector<NodeInfo> &nodes,
                                               std::atomic<size_t> &index);
    std::optional<NodeInfo> select_least_loaded(const std::vector<NodeInfo> &nodes);
    std::optional<NodeInfo> select_random(const std::vector<NodeInfo> &nodes);

    // Select decode node with available slots
    std::optional<NodeInfo> select_decode_node_with_slot();
};
