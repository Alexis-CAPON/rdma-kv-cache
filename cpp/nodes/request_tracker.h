#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cstdint>

/**
 * RequestTracker - Manages request lifecycle and KV cache transfer state
 *
 * Responsibilities:
 * - Assign unique sequence numbers to requests (for imm_data encoding)
 * - Track request state machine (ASSIGNED → DECODING → COMPLETED)
 * - Track chunk arrival for each request
 * - Map between request_id and sequence numbers
 *
 * Thread-safe for concurrent worker access.
 */
class RequestTracker
{
public:
    RequestTracker();
    ~RequestTracker() = default;

    // ========================================
    // Request State Machine
    // ========================================

    enum class State
    {
        ASSIGNED,           // Orchestrator assigned this request to decode node
        WAITING_FOR_PREFILL,// Waiting for prefill to complete
        WAITING_FOR_KV,     // Prefill done, waiting for KV transfer to start
        RECEIVING_KV,       // Actively receiving KV cache chunks
        READY_FOR_DECODE,   // All chunks received, ready to trigger vLLM decode
        DECODING,           // vLLM decode in progress
        COMPLETED,          // Decode complete, response sent
        FAILED              // Request failed at some stage
    };

    static std::string state_to_string(State state);

    // ========================================
    // Request Info
    // ========================================

    struct KvMetadata
    {
        int num_tokens;              // Number of tokens prefilled
        int total_chunks;            // Number of KV cache chunks
        size_t chunk_size;           // Size of each chunk in bytes

        // Chunk tracking
        std::vector<bool> chunks_received;  // Bitmap of received chunks
        int chunks_arrived_count;           // Count of received chunks

        // GPU buffer allocation
        void* gpu_buffer_ptr;        // Base pointer in decode GPU memory
        size_t gpu_buffer_size;      // Total size allocated

        // Timing metrics
        std::chrono::steady_clock::time_point prefill_complete_ts;
        std::chrono::steady_clock::time_point first_chunk_ts;
        std::chrono::steady_clock::time_point last_chunk_ts;
        std::chrono::steady_clock::time_point decode_start_ts;
        std::chrono::steady_clock::time_point decode_complete_ts;
    };

    struct RequestInfo
    {
        std::string request_id;
        std::string prefill_node_id;
        uint16_t seq_num;            // Sequence number for imm_data encoding

        State state;
        KvMetadata kv_meta;

        // Request metadata
        std::string prompt;
        int max_output_tokens;

        // Error handling
        std::string error_message;
    };

    // ========================================
    // Lifecycle Operations
    // ========================================

    /**
     * Add a new request to tracker
     * @param request_id Unique request ID
     * @param prefill_node_id Which prefill node is handling this
     * @return Assigned sequence number (for imm_data encoding)
     */
    uint16_t add_request(const std::string& request_id,
                        const std::string& prefill_node_id);

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
    // KV Transfer Management
    // ========================================

    /**
     * Initialize KV metadata when PREFILL_COMPLETE arrives
     */
    bool init_kv_transfer(const std::string& request_id,
                         int num_tokens,
                         int total_chunks,
                         size_t chunk_size,
                         void* gpu_buffer_ptr,
                         size_t gpu_buffer_size);

    /**
     * Mark a chunk as received
     * @return true if this was the last chunk (transfer complete)
     */
    bool mark_chunk_received(const std::string& request_id,
                            uint16_t chunk_index);

    /**
     * Get chunk arrival progress
     */
    bool get_progress(const std::string& request_id,
                     int& received,
                     int& total) const;

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

    // ========================================
    // Chunk ID Encoding/Decoding
    // ========================================

    /**
     * Encode request sequence number and chunk index into imm_data
     */
    static uint32_t encode_chunk_id(uint16_t request_seq, uint16_t chunk_index);

    /**
     * Decode imm_data back to request sequence and chunk index
     */
    static void decode_chunk_id(uint32_t imm_data,
                               uint16_t& request_seq,
                               uint16_t& chunk_index);

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
    mutable std::mutex mutex_;

    // Request tracking
    std::unordered_map<std::string, RequestInfo> requests_;

    // Sequence number management
    std::atomic<uint16_t> next_seq_num_;
    std::unordered_map<std::string, uint16_t> request_to_seq_;
    std::unordered_map<uint16_t, std::string> seq_to_request_;

    // Statistics
    std::atomic<uint64_t> total_requests_;
    std::atomic<uint64_t> completed_requests_;
    std::atomic<uint64_t> failed_requests_;
};
