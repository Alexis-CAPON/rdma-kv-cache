#pragma once
#include "cpp/common/types.h"
#include <unordered_map>
#include <shared_mutex>
#include <optional>
#include <chrono>

/**
 * RequestTracker - Tracks request lifecycle and timing
 *
 * Stores:
 * - Request info (prompt, assigned nodes, etc.)
 * - Status (PENDING, PREFILLING, DECODING, etc.)
 * - Client FD (for sending response)
 * - Timing information
 * - Response/error data
 */
class RequestTracker
{
public:
    RequestTracker() = default;
    ~RequestTracker() = default;

    // Non-copyable
    RequestTracker(const RequestTracker &) = delete;
    RequestTracker &operator=(const RequestTracker &) = delete;

    // ========================================
    // Request Lifecycle
    // ========================================

    /**
     * Start tracking a new request
     * Stores client_fd for later response delivery
     */
    void track_request(const RequestInfo &request, int client_fd);

    /**
     * Update request status
     * Automatically updates relevant timestamps
     */
    void update_status(const std::string &request_id, RequestStatus status);

    /**
     * Mark prefill phase complete
     */
    void mark_prefill_complete(const std::string &request_id);

    /**
     * Mark decode phase complete with response
     */
    void mark_decode_complete(const std::string &request_id,
                              const std::string &response);

    /**
     * Mark request as failed
     */
    void mark_failed(const std::string &request_id, const std::string &error);

    // ========================================
    // Queries
    // ========================================

    /**
     * Get complete request info
     */
    std::optional<RequestInfo> get_request(const std::string &request_id) const;

    /**
     * Get request status
     */
    std::optional<RequestStatus> get_status(const std::string &request_id) const;

    /**
     * Get client FD for sending response
     */
    std::optional<int> get_client_fd(const std::string &request_id) const;

    /**
     * Get response text (if completed)
     */
    std::optional<std::string> get_response(const std::string &request_id) const;

    /**
     * Get error message (if failed)
     */
    std::optional<std::string> get_error(const std::string &request_id) const;

    /**
     * Get requests that have exceeded timeout
     */
    std::vector<std::string> get_timed_out_requests(int timeout_ms) const;

    // ========================================
    // Cleanup
    // ========================================

    /**
     * Remove a request from tracking
     */
    void remove_request(const std::string &request_id);

    /**
     * Cleanup old completed/failed requests
     * Returns number of requests removed
     */
    size_t cleanup_old_requests(int max_age_ms);

    // ========================================
    // Statistics
    // ========================================

    struct Stats
    {
        size_t total_requests;
        size_t pending_requests;
        size_t prefilling_requests;
        size_t decoding_requests;
        size_t completed_requests;
        size_t failed_requests;
        double avg_prefill_latency_ms;
        double avg_decode_latency_ms;
        double avg_total_latency_ms;
    };

    Stats get_stats() const;

private:
    mutable std::shared_mutex mutex_;

    struct TrackedRequest
    {
        RequestInfo info;
        RequestStatus status;
        int client_fd;
        std::string response;
        std::string error;
    };

    std::unordered_map<std::string, TrackedRequest> requests_;

    // Helper to calculate latencies
    double calculate_latency_ms(uint64_t start, uint64_t end) const;
};
