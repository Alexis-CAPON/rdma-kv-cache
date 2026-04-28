#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cstdint>

/**
 * RequestTrackerLayer - Manages request lifecycle with layer-based KV cache transfer
 *
 * Responsibilities:
 * - Assign unique sequence numbers to requests (for imm_data encoding)
 * - Track request state machine (ASSIGNED → RECEIVING_KV → READY_FOR_DECODE → DECODING → COMPLETED)
 * - Track layer arrival for each request (replaces chunk tracking)
 * - Map between request_id and sequence numbers
 * - Calculate RDMA offsets based on slot_id + layer_id
 *
 * Thread-safe for concurrent worker access.
 */
class RequestTrackerLayer
{
public:
    // Memory layout configuration (passed from orchestrator via config)
    struct MemoryLayout
    {
        size_t buffer_size_bytes;      // Total buffer (e.g., 16GB)
        int max_concurrent_requests;   // Max slots (e.g., 8)
        int num_layers;                // Transformer layers (e.g., 32)
        size_t layer_size_bytes;       // Per-layer size (e.g., 128MB)

        size_t get_request_size() const {
            return num_layers * layer_size_bytes;  // 4GB per request
        }

        size_t get_slot_base_offset(int slot) const {
            return slot * get_request_size();
        }

        size_t get_layer_offset(int slot, int layer_id) const {
            return get_slot_base_offset(slot) + (layer_id * layer_size_bytes);
        }

        bool validate_offset(size_t offset, size_t length) const {
            return (offset + length) <= buffer_size_bytes;
        }
    };

    explicit RequestTrackerLayer(MemoryLayout layout = MemoryLayout{
        .buffer_size_bytes = 16UL * 1024 * 1024 * 1024,  // 16GB
        .max_concurrent_requests = 8,
        .num_layers = 32,
        .layer_size_bytes = 128UL * 1024 * 1024  // 128MB
    });
    ~RequestTrackerLayer() = default;

    // ========================================
    // Request State Machine
    // ========================================

    enum class State
    {
        ASSIGNED,           // Orchestrator assigned this request to decode node
        WAITING_FOR_PREFILL,// Waiting for prefill to complete
        WAITING_FOR_KV,     // Prefill done, waiting for KV transfer to start
        RECEIVING_KV,       // Actively receiving KV cache layers
        READY_FOR_DECODE,   // All layers received, ready to trigger vLLM decode
        DECODING,           // vLLM decode in progress
        COMPLETED,          // Decode complete, response sent
        FAILED              // Request failed at some stage
    };

    static std::string state_to_string(State state);

    // ========================================
    // Layer Metadata
    // ========================================

    struct LayerMetadata
    {
        int layer_id;
        size_t offset;           // Absolute offset in buffer
        size_t size;             // Layer data size
        bool received;           // Has this layer arrived?
        std::chrono::steady_clock::time_point arrival_ts;
    };

    // ========================================
    // Request Info
    // ========================================

    struct RequestInfo
    {
        std::string request_id;
        std::string prefill_node_id;
        uint16_t seq_num;        // For imm_data encoding

        // Slot-based allocation
        int slot_id;             // 0-7 for max_concurrent=8
        size_t base_offset;      // Precomputed: slot_id * request_size

        // Layer tracking (replaces chunk tracking)
        int num_layers;
        std::vector<LayerMetadata> layers;
        int layers_received_count;

        // State machine
        State state;

        // Timing
        std::chrono::steady_clock::time_point assigned_ts;
        std::chrono::steady_clock::time_point first_layer_ts;
        std::chrono::steady_clock::time_point all_layers_ts;
        std::chrono::steady_clock::time_point decode_start_ts;
        std::chrono::steady_clock::time_point decode_complete_ts;

        // Metadata
        std::string prompt;
        int num_tokens;
        int max_output_tokens;
        std::string error_message;
    };

    // ========================================
    // Lifecycle Operations
    // ========================================

    /**
     * Add a new request to tracker with assigned slot
     * @param request_id Unique request ID
     * @param prefill_node_id Which prefill node is handling this
     * @param assigned_slot Slot allocated by router (0-7)
     * @param num_layers Number of transformer layers
     * @return Assigned sequence number (for imm_data encoding)
     */
    uint16_t add_request(const std::string& request_id,
                        const std::string& prefill_node_id,
                        int assigned_slot,
                        int num_layers);

    /**
     * Update request state
     */
    bool update_state(const std::string& request_id, State new_state);

    /**
     * Get request info (thread-safe copy)
     */
    bool get_request(const std::string& request_id, RequestInfo& info) const;

    /**
     * Remove completed/failed request
     */
    void remove_request(const std::string& request_id);

    // ========================================
    // Layer Tracking
    // ========================================

    /**
     * Mark a layer as received (called by RDMA poller)
     * @param request_id Request identifier
     * @param layer_id Layer index (0-31)
     * @return true if this was the last layer (transfer complete)
     */
    bool mark_layer_received(const std::string& request_id, int layer_id);

    /**
     * Check if specific layer has been received
     */
    bool is_layer_ready(const std::string& request_id, int layer_id) const;

    /**
     * Get layer arrival progress
     */
    bool get_progress(const std::string& request_id, int& received, int& total) const;

    /**
     * Get offset for specific layer
     */
    size_t get_layer_offset(const std::string& request_id, int layer_id) const;

    // ========================================
    // Sequence Number Resolution
    // ========================================

    /**
     * Resolve sequence number back to request_id
     * Used when decoding imm_data from RDMA completion
     */
    std::string resolve_seq_num(uint16_t seq_num) const;

    /**
     * Get sequence number for a request
     */
    uint16_t get_seq_num(const std::string& request_id) const;

    /**
     * Get slot ID for request
     */
    int get_slot_id(const std::string& request_id) const;

    // ========================================
    // Layer ID Encoding/Decoding (for imm_data)
    // ========================================

    /**
     * Encode request sequence number and layer ID into imm_data
     * Format: [16 bits: seq_num] [16 bits: layer_id]
     */
    static uint32_t encode_layer_id(uint16_t request_seq, uint16_t layer_id);

    /**
     * Decode imm_data back to request sequence and layer ID
     */
    static void decode_layer_id(uint32_t imm_data,
                               uint16_t& request_seq,
                               uint16_t& layer_id);

    // ========================================
    // Statistics & Monitoring
    // ========================================

    struct Stats
    {
        int total_requests;
        int active_requests;
        int completed_requests;
        int failed_requests;

        // By state
        int assigned_count;
        int waiting_for_prefill_count;
        int waiting_for_kv_count;
        int receiving_kv_count;
        int ready_for_decode_count;
        int decoding_count;
    };

    Stats get_stats() const;

    /**
     * Get list of all request IDs in a specific state
     */
    std::vector<std::string> get_requests_in_state(State state) const;

private:
    MemoryLayout layout_;
    mutable std::mutex mutex_;

    // Request tracking
    std::unordered_map<std::string, RequestInfo> requests_;

    // Sequence number management
    std::atomic<uint16_t> next_seq_num_{1};
    std::unordered_map<std::string, uint16_t> request_to_seq_;
    std::unordered_map<uint16_t, std::string> seq_to_request_;

    // Statistics
    std::atomic<uint64_t> total_requests_{0};
    std::atomic<uint64_t> completed_requests_{0};
    std::atomic<uint64_t> failed_requests_{0};
};
