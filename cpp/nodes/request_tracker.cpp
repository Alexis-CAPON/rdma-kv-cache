#include "cpp/nodes/request_tracker.h"
#include "cpp/common/logger.h"
#include <arpa/inet.h>

RequestTracker::RequestTracker()
    : next_seq_num_(1),
      total_requests_(0),
      completed_requests_(0),
      failed_requests_(0)
{
    Logger::info("RequestTracker: Initialized");
}

// ══════════════════════════════════════════════════════════════════════════════
//  Lifecycle Operations
// ══════════════════════════════════════════════════════════════════════════════

uint16_t RequestTracker::add_request(const std::string& request_id,
                                     const std::string& prefill_node_id)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // Check if already exists
    if (requests_.find(request_id) != requests_.end())
    {
        Logger::warn("RequestTracker: Request " + request_id + " already exists");
        return request_to_seq_[request_id];
    }

    // Assign sequence number
    uint16_t seq = next_seq_num_.fetch_add(1);

    // Handle wraparound (unlikely but possible)
    if (seq == 0)
    {
        seq = next_seq_num_.fetch_add(1);
    }

    // Create request info
    RequestInfo info;
    info.request_id = request_id;
    info.prefill_node_id = prefill_node_id;
    info.seq_num = seq;
    info.state = State::ASSIGNED;

    // Initialize KV metadata
    info.kv_meta.num_tokens = 0;
    info.kv_meta.total_chunks = 0;
    info.kv_meta.chunk_size = 0;
    info.kv_meta.chunks_arrived_count = 0;
    info.kv_meta.gpu_buffer_ptr = nullptr;
    info.kv_meta.gpu_buffer_size = 0;

    // Store mappings
    requests_[request_id] = info;
    request_to_seq_[request_id] = seq;
    seq_to_request_[seq] = request_id;

    total_requests_.fetch_add(1);

    Logger::info("RequestTracker: Added request " + request_id +
                 " (seq=" + std::to_string(seq) + ")" +
                 " from prefill " + prefill_node_id);

    return seq;
}

bool RequestTracker::update_state(const std::string& request_id, State new_state)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = requests_.find(request_id);
    if (it == requests_.end())
    {
        Logger::error("RequestTracker: Request " + request_id + " not found");
        return false;
    }

    State old_state = it->second.state;
    it->second.state = new_state;

    Logger::debug("RequestTracker: " + request_id + " state: " +
                  state_to_string(old_state) + " → " + state_to_string(new_state));

    // Update counters
    if (new_state == State::COMPLETED)
    {
        completed_requests_.fetch_add(1);
    }
    else if (new_state == State::FAILED)
    {
        failed_requests_.fetch_add(1);
    }

    return true;
}

bool RequestTracker::get_request(const std::string& request_id,
                                 RequestInfo& info) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = requests_.find(request_id);
    if (it == requests_.end())
    {
        return false;
    }

    info = it->second;  // Copy
    return true;
}

void RequestTracker::remove_request(const std::string& request_id)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = requests_.find(request_id);
    if (it == requests_.end())
    {
        return;
    }

    uint16_t seq = it->second.seq_num;

    // Remove from all mappings
    requests_.erase(it);
    request_to_seq_.erase(request_id);
    seq_to_request_.erase(seq);

    Logger::debug("RequestTracker: Removed request " + request_id);
}

// ══════════════════════════════════════════════════════════════════════════════
//  KV Transfer Management
// ══════════════════════════════════════════════════════════════════════════════

bool RequestTracker::init_kv_transfer(const std::string& request_id,
                                      int num_tokens,
                                      int total_chunks,
                                      size_t chunk_size,
                                      void* gpu_buffer_ptr,
                                      size_t gpu_buffer_size)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = requests_.find(request_id);
    if (it == requests_.end())
    {
        Logger::error("RequestTracker: Cannot init KV transfer for unknown request " + request_id);
        return false;
    }

    RequestInfo& req = it->second;

    // Initialize metadata
    req.kv_meta.num_tokens = num_tokens;
    req.kv_meta.total_chunks = total_chunks;
    req.kv_meta.chunk_size = chunk_size;
    req.kv_meta.chunks_received.resize(total_chunks, false);
    req.kv_meta.chunks_arrived_count = 0;
    req.kv_meta.gpu_buffer_ptr = gpu_buffer_ptr;
    req.kv_meta.gpu_buffer_size = gpu_buffer_size;
    req.kv_meta.prefill_complete_ts = std::chrono::steady_clock::now();

    Logger::info("RequestTracker: Initialized KV transfer for " + request_id +
                 " (" + std::to_string(total_chunks) + " chunks, " +
                 std::to_string(gpu_buffer_size / (1024 * 1024)) + " MB)");

    return true;
}

bool RequestTracker::mark_chunk_received(const std::string& request_id,
                                        uint16_t chunk_index)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = requests_.find(request_id);
    if (it == requests_.end())
    {
        Logger::warn("RequestTracker: mark_chunk_received for unknown request " + request_id);
        return false;
    }

    RequestInfo& req = it->second;

    // Validate chunk index
    if (chunk_index >= req.kv_meta.chunks_received.size())
    {
        Logger::error("RequestTracker: Invalid chunk index " +
                      std::to_string(chunk_index) + " for " + request_id +
                      " (expected 0-" + std::to_string(req.kv_meta.total_chunks - 1) + ")");
        return false;
    }

    // Check if already received (duplicate)
    if (req.kv_meta.chunks_received[chunk_index])
    {
        Logger::warn("RequestTracker: Duplicate chunk " +
                     std::to_string(chunk_index) + " for " + request_id);
        return false;
    }

    // Mark as received
    req.kv_meta.chunks_received[chunk_index] = true;
    req.kv_meta.chunks_arrived_count++;

    // Record timing
    if (req.kv_meta.chunks_arrived_count == 1)
    {
        req.kv_meta.first_chunk_ts = std::chrono::steady_clock::now();
    }

    Logger::debug("RequestTracker: " + request_id + " chunk " +
                  std::to_string(chunk_index) + " received (" +
                  std::to_string(req.kv_meta.chunks_arrived_count) + "/" +
                  std::to_string(req.kv_meta.total_chunks) + ")");

    // Check if transfer complete
    bool transfer_complete = (req.kv_meta.chunks_arrived_count ==
                             req.kv_meta.total_chunks);

    if (transfer_complete)
    {
        req.kv_meta.last_chunk_ts = std::chrono::steady_clock::now();

        // Calculate transfer time
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            req.kv_meta.last_chunk_ts - req.kv_meta.first_chunk_ts);

        Logger::info("RequestTracker: KV transfer complete for " + request_id +
                     " (took " + std::to_string(duration.count()) + " ms)");
    }

    return transfer_complete;
}

bool RequestTracker::get_progress(const std::string& request_id,
                                  int& received,
                                  int& total) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = requests_.find(request_id);
    if (it == requests_.end())
    {
        return false;
    }

    received = it->second.kv_meta.chunks_arrived_count;
    total = it->second.kv_meta.total_chunks;
    return true;
}

// ══════════════════════════════════════════════════════════════════════════════
//  Sequence Number Resolution
// ══════════════════════════════════════════════════════════════════════════════

std::string RequestTracker::resolve_seq_num(uint16_t seq_num) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = seq_to_request_.find(seq_num);
    if (it == seq_to_request_.end())
    {
        return "";  // Unknown sequence number
    }

    return it->second;
}

uint16_t RequestTracker::get_seq_num(const std::string& request_id) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = request_to_seq_.find(request_id);
    if (it == request_to_seq_.end())
    {
        return 0;  // Not found
    }

    return it->second;
}

// ══════════════════════════════════════════════════════════════════════════════
//  Chunk ID Encoding/Decoding
// ══════════════════════════════════════════════════════════════════════════════

uint32_t RequestTracker::encode_chunk_id(uint16_t request_seq, uint16_t chunk_index)
{
    return (static_cast<uint32_t>(request_seq) << 16) | chunk_index;
}

void RequestTracker::decode_chunk_id(uint32_t imm_data,
                                     uint16_t& request_seq,
                                     uint16_t& chunk_index)
{
    request_seq = (imm_data >> 16) & 0xFFFF;
    chunk_index = imm_data & 0xFFFF;
}

// ══════════════════════════════════════════════════════════════════════════════
//  Statistics & Monitoring
// ══════════════════════════════════════════════════════════════════════════════

RequestTracker::Stats RequestTracker::get_stats() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    Stats stats;
    stats.total_requests = total_requests_.load();
    stats.completed_requests = completed_requests_.load();
    stats.failed_requests = failed_requests_.load();
    stats.active_requests = requests_.size();

    // Count by state
    stats.assigned_count = 0;
    stats.waiting_for_prefill_count = 0;
    stats.waiting_for_kv_count = 0;
    stats.receiving_kv_count = 0;
    stats.ready_for_decode_count = 0;
    stats.decoding_count = 0;

    for (const auto& [id, req] : requests_)
    {
        switch (req.state)
        {
        case State::ASSIGNED:
            stats.assigned_count++;
            break;
        case State::WAITING_FOR_PREFILL:
            stats.waiting_for_prefill_count++;
            break;
        case State::WAITING_FOR_KV:
            stats.waiting_for_kv_count++;
            break;
        case State::RECEIVING_KV:
            stats.receiving_kv_count++;
            break;
        case State::READY_FOR_DECODE:
            stats.ready_for_decode_count++;
            break;
        case State::DECODING:
            stats.decoding_count++;
            break;
        default:
            break;
        }
    }

    return stats;
}

std::vector<std::string> RequestTracker::get_requests_in_state(State state) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::string> result;
    for (const auto& [id, req] : requests_)
    {
        if (req.state == state)
        {
            result.push_back(id);
        }
    }

    return result;
}

std::string RequestTracker::state_to_string(State state)
{
    switch (state)
    {
    case State::ASSIGNED:
        return "ASSIGNED";
    case State::WAITING_FOR_PREFILL:
        return "WAITING_FOR_PREFILL";
    case State::WAITING_FOR_KV:
        return "WAITING_FOR_KV";
    case State::RECEIVING_KV:
        return "RECEIVING_KV";
    case State::READY_FOR_DECODE:
        return "READY_FOR_DECODE";
    case State::DECODING:
        return "DECODING";
    case State::COMPLETED:
        return "COMPLETED";
    case State::FAILED:
        return "FAILED";
    default:
        return "UNKNOWN";
    }
}
