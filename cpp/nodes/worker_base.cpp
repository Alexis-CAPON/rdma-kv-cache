
#include "cpp/common/logger.h"
#include "cpp/nodes/worker_base.h"
#include <chrono>
#include <algorithm>
#include <cstring>
#include <functional>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Constructor for prefill/decode nodes
WorkerBase::WorkerBase(
    uint32_t worker_id,
    const Config &config,
    const std::string &node_id,
    std::atomic<uint64_t> *server_ops_counter)
    : worker_id_(worker_id),
      node_id_(node_id),
      config_(config),
      server_ops_counter_(server_ops_counter),
      events_processed_(0)
{
    Logger::info("Worker Base" + std::to_string(worker_id_) + " initialized for Prefill/Decode node");
}

WorkerBase::~WorkerBase()
{
    Logger::info("Worker Base " + std::to_string(worker_id_) + " destroyed (" +
                 std::to_string(events_processed_) + " events processed)");
}

// ============================================================================
// Helper Functions
// ============================================================================

uint64_t WorkerBase::generate_transaction_id()
{
    // Generate globally unique transaction ID across all nodes and workers
    // Format: [32 bits: timestamp_low] [24 bits: node_id] [8 bits: worker_id]
    // Using timestamp ensures uniqueness even if counters reset
    auto now = std::chrono::steady_clock::now();
    uint64_t timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                             now.time_since_epoch())
                             .count();

    // Encode: timestamp (low 32 bits) | node_id hash (24 bits) | worker_id (8 bits)
    uint64_t node_hash = std::hash<std::string>{}(node_id_) & 0xFFFFFF;
    return (timestamp << 32) | (node_hash << 8) | (worker_id_ & 0xFF);
}