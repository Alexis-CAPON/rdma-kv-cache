#include "cpp/nodes/request_tracker_layer.h"
#include "cpp/common/logger.h"
#include <stdexcept>

RequestTrackerLayer::RequestTrackerLayer(MemoryLayout layout)
    : layout_(layout)
{
    Logger::info("RequestTrackerLayer: Initialized with buffer_size=" +
                 std::to_string(layout_.buffer_size_bytes / (1024 * 1024)) + "MB" +
                 ", max_requests=" + std::to_string(layout_.max_concurrent_requests) +
                 ", num_layers=" + std::to_string(layout_.num_layers) +
                 ", layer_size=" + std::to_string(layout_.layer_size_bytes / (1024 * 1024)) + "MB");
}

std::string RequestTrackerLayer::state_to_string(State state)
{
    switch (state)
    {
    case State::ASSIGNED: return "ASSIGNED";
    case State::WAITING_FOR_PREFILL: return "WAITING_FOR_PREFILL";
    case State::WAITING_FOR_KV: return "WAITING_FOR_KV";
    case State::RECEIVING_KV: return "RECEIVING_KV";
    case State::READY_FOR_DECODE: return "READY_FOR_DECODE";
    case State::DECODING: return "DECODING";
    case State::COMPLETED: return "COMPLETED";
    case State::FAILED: return "FAILED";
    default: return "UNKNOWN";
    }
}

// ============================================================================
// Lifecycle Operations
// ============================================================================

uint16_t RequestTrackerLayer::add_request(
    const std::string& request_id,
    const std::string& prefill_node_id,
    int assigned_slot,
    int num_layers)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // Validate slot
    if (assigned_slot < 0 || assigned_slot >= layout_.max_concurrent_requests) {
        throw std::invalid_argument("Invalid slot: " + std::to_string(assigned_slot));
    }

    // Check slot not already in use
    for (const auto& [id, info] : requests_) {
        if (info.slot_id == assigned_slot) {
            throw std::runtime_error("Slot " + std::to_string(assigned_slot) +
                                     " already in use by " + id);
        }
    }

    // Assign sequence number
    uint16_t seq_num = next_seq_num_++;

    // Calculate base offset
    size_t base_offset = layout_.get_slot_base_offset(assigned_slot);

    // Initialize layer metadata
    std::vector<LayerMetadata> layers(num_layers);
    for (int i = 0; i < num_layers; i++) {
        layers[i] = LayerMetadata{
            .layer_id = i,
            .offset = layout_.get_layer_offset(assigned_slot, i),
            .size = layout_.layer_size_bytes,
            .received = false
        };
    }

    // Create request info
    RequestInfo info{
        .request_id = request_id,
        .prefill_node_id = prefill_node_id,
        .seq_num = seq_num,
        .slot_id = assigned_slot,
        .base_offset = base_offset,
        .num_layers = num_layers,
        .layers = std::move(layers),
        .layers_received_count = 0,
        .state = State::ASSIGNED,
        .assigned_ts = std::chrono::steady_clock::now(),
        .num_tokens = 0,
        .max_output_tokens = 0
    };

    requests_[request_id] = std::move(info);
    request_to_seq_[request_id] = seq_num;
    seq_to_request_[seq_num] = request_id;
    total_requests_++;

    Logger::info("RequestTrackerLayer: Added request " + request_id +
                 " slot=" + std::to_string(assigned_slot) +
                 " seq_num=" + std::to_string(seq_num) +
                 " base_offset=0x" + std::to_string(base_offset) +
                 " num_layers=" + std::to_string(num_layers));

    return seq_num;
}

bool RequestTrackerLayer::update_state(const std::string& request_id, State new_state)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = requests_.find(request_id);
    if (it == requests_.end()) {
        Logger::warning("RequestTrackerLayer: Cannot update state for unknown request " + request_id);
        return false;
    }

    State old_state = it->second.state;
    it->second.state = new_state;

    Logger::debug("RequestTrackerLayer: Request " + request_id +
                  " state transition: " + state_to_string(old_state) +
                  " → " + state_to_string(new_state));

    // Update statistics
    if (new_state == State::COMPLETED) {
        completed_requests_++;
        it->second.decode_complete_ts = std::chrono::steady_clock::now();
    } else if (new_state == State::FAILED) {
        failed_requests_++;
    } else if (new_state == State::DECODING) {
        it->second.decode_start_ts = std::chrono::steady_clock::now();
    }

    return true;
}

bool RequestTrackerLayer::get_request(const std::string& request_id, RequestInfo& info) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = requests_.find(request_id);
    if (it == requests_.end()) {
        return false;
    }

    info = it->second;
    return true;
}

void RequestTrackerLayer::remove_request(const std::string& request_id)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = requests_.find(request_id);
    if (it == requests_.end()) {
        Logger::warning("RequestTrackerLayer: Cannot remove unknown request " + request_id);
        return;
    }

    uint16_t seq_num = it->second.seq_num;

    requests_.erase(it);
    request_to_seq_.erase(request_id);
    seq_to_request_.erase(seq_num);

    Logger::info("RequestTrackerLayer: Removed request " + request_id);
}

// ============================================================================
// Layer Tracking
// ============================================================================

bool RequestTrackerLayer::mark_layer_received(const std::string& request_id, int layer_id)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = requests_.find(request_id);
    if (it == requests_.end()) {
        Logger::warning("RequestTrackerLayer: Cannot mark layer for unknown request " + request_id);
        return false;
    }

    RequestInfo& info = it->second;

    if (layer_id < 0 || layer_id >= info.num_layers) {
        Logger::error("RequestTrackerLayer: Invalid layer_id " + std::to_string(layer_id) +
                      " for request " + request_id + " (max=" + std::to_string(info.num_layers) + ")");
        return false;
    }

    LayerMetadata& layer = info.layers[layer_id];

    if (layer.received) {
        Logger::warning("RequestTrackerLayer: Layer " + std::to_string(layer_id) +
                        " already received for request " + request_id);
        return false;
    }

    // Mark received
    layer.received = true;
    layer.arrival_ts = std::chrono::steady_clock::now();
    info.layers_received_count++;

    // Track first layer arrival
    if (info.layers_received_count == 1) {
        info.first_layer_ts = layer.arrival_ts;
        info.state = State::RECEIVING_KV;
    }

    Logger::debug("RequestTrackerLayer: Request " + request_id +
                  " layer " + std::to_string(layer_id) + " received (" +
                  std::to_string(info.layers_received_count) + "/" +
                  std::to_string(info.num_layers) + ")");

    // Check if all layers received
    bool all_received = (info.layers_received_count == info.num_layers);
    if (all_received) {
        info.all_layers_ts = std::chrono::steady_clock::now();
        info.state = State::READY_FOR_DECODE;

        auto transfer_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            info.all_layers_ts - info.first_layer_ts).count();

        Logger::info("RequestTrackerLayer: Request " + request_id +
                     " all layers received (transfer took " +
                     std::to_string(transfer_duration) + "ms)");
    }

    return all_received;
}

bool RequestTrackerLayer::is_layer_ready(const std::string& request_id, int layer_id) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = requests_.find(request_id);
    if (it == requests_.end()) {
        return false;
    }

    if (layer_id < 0 || layer_id >= it->second.num_layers) {
        return false;
    }

    return it->second.layers[layer_id].received;
}

bool RequestTrackerLayer::get_progress(const std::string& request_id, int& received, int& total) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = requests_.find(request_id);
    if (it == requests_.end()) {
        return false;
    }

    received = it->second.layers_received_count;
    total = it->second.num_layers;
    return true;
}

size_t RequestTrackerLayer::get_layer_offset(const std::string& request_id, int layer_id) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = requests_.find(request_id);
    if (it == requests_.end()) {
        throw std::runtime_error("Request not found: " + request_id);
    }

    const RequestInfo& info = it->second;

    if (layer_id < 0 || layer_id >= info.num_layers) {
        throw std::invalid_argument("Invalid layer_id: " + std::to_string(layer_id));
    }

    return info.layers[layer_id].offset;
}

// ============================================================================
// Sequence Number Resolution
// ============================================================================

std::string RequestTrackerLayer::resolve_seq_num(uint16_t seq_num) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = seq_to_request_.find(seq_num);
    if (it == seq_to_request_.end()) {
        return "";
    }

    return it->second;
}

uint16_t RequestTrackerLayer::get_seq_num(const std::string& request_id) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = request_to_seq_.find(request_id);
    if (it == request_to_seq_.end()) {
        return 0;
    }

    return it->second;
}

int RequestTrackerLayer::get_slot_id(const std::string& request_id) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = requests_.find(request_id);
    if (it == requests_.end()) {
        return -1;
    }

    return it->second.slot_id;
}

// ============================================================================
// Layer ID Encoding/Decoding
// ============================================================================

uint32_t RequestTrackerLayer::encode_layer_id(uint16_t request_seq, uint16_t layer_id)
{
    return (static_cast<uint32_t>(request_seq) << 16) | layer_id;
}

void RequestTrackerLayer::decode_layer_id(uint32_t imm_data,
                                         uint16_t& request_seq,
                                         uint16_t& layer_id)
{
    request_seq = static_cast<uint16_t>(imm_data >> 16);
    layer_id = static_cast<uint16_t>(imm_data & 0xFFFF);
}

// ============================================================================
// Statistics & Monitoring
// ============================================================================

RequestTrackerLayer::Stats RequestTrackerLayer::get_stats() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    Stats stats{
        .total_requests = static_cast<int>(total_requests_.load()),
        .active_requests = static_cast<int>(requests_.size()),
        .completed_requests = static_cast<int>(completed_requests_.load()),
        .failed_requests = static_cast<int>(failed_requests_.load()),
        .assigned_count = 0,
        .waiting_for_prefill_count = 0,
        .waiting_for_kv_count = 0,
        .receiving_kv_count = 0,
        .ready_for_decode_count = 0,
        .decoding_count = 0
    };

    // Count by state
    for (const auto& [id, info] : requests_) {
        switch (info.state) {
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

std::vector<std::string> RequestTrackerLayer::get_requests_in_state(State state) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::string> result;
    for (const auto& [id, info] : requests_) {
        if (info.state == state) {
            result.push_back(id);
        }
    }

    return result;
}
