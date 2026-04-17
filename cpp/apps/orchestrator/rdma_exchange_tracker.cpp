#include "cpp/apps/orchestrator/rdma_exchange_tracker.h"
#include "cpp/common/logger.h"

RdmaExchangeTracker::RdmaExchangeTracker(int expected_prefill_nodes,
                                         int expected_decode_nodes)
    : expected_prefill_nodes_(expected_prefill_nodes),
      expected_decode_nodes_(expected_decode_nodes)
{
    Logger::info("RdmaExchangeTracker: Expecting " +
                 std::to_string(expected_prefill_nodes) + " prefill nodes, " +
                 std::to_string(expected_decode_nodes) + " decode nodes");
}

// ============================================================================
// Registration Phase
// ============================================================================

bool RdmaExchangeTracker::register_node(const std::string &node_id, NodeRole role)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);

    // Check if already registered
    if (registered_nodes_.count(node_id) > 0)
    {
        Logger::warning("RdmaExchangeTracker: Node " + node_id + " already registered");
        return all_nodes_registered();
    }

    // Add to registered sets
    registered_nodes_.insert(node_id);

    if (role == NodeRole::PREFILL)
    {
        registered_prefill_.insert(node_id);
        Logger::info("RdmaExchangeTracker: Registered prefill node " + node_id +
                     " (" + std::to_string(registered_prefill_.size()) + "/" +
                     std::to_string(expected_prefill_nodes_) + ")");
    }
    else if (role == NodeRole::DECODE)
    {
        registered_decode_.insert(node_id);
        Logger::info("RdmaExchangeTracker: Registered decode node " + node_id +
                     " (" + std::to_string(registered_decode_.size()) + "/" +
                     std::to_string(expected_decode_nodes_) + ")");
    }

    // Check if all nodes registered
    bool all_registered = (registered_prefill_.size() == expected_prefill_nodes_) &&
                          (registered_decode_.size() == expected_decode_nodes_);

    if (all_registered)
    {
        Logger::info("RdmaExchangeTracker: All nodes registered!");
        cv_.notify_all();
    }

    return all_registered;
}

bool RdmaExchangeTracker::all_nodes_registered() const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return (registered_prefill_.size() == expected_prefill_nodes_) &&
           (registered_decode_.size() == expected_decode_nodes_);
}

bool RdmaExchangeTracker::wait_for_all_nodes(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);

    bool result = cv_.wait_for(lock, timeout, [this]()
                               { return (registered_prefill_.size() == expected_prefill_nodes_) &&
                                        (registered_decode_.size() == expected_decode_nodes_); });

    if (!result)
    {
        Logger::warning("RdmaExchangeTracker: Timeout waiting for all nodes. " +
                        std::to_string(registered_prefill_.size()) + "/" +
                        std::to_string(expected_prefill_nodes_) + " prefill, " +
                        std::to_string(registered_decode_.size()) + "/" +
                        std::to_string(expected_decode_nodes_) + " decode");
    }

    return result;
}

std::unordered_set<std::string> RdmaExchangeTracker::get_registered_nodes() const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return registered_nodes_;
}

// ============================================================================
// Ready Phase
// ============================================================================

bool RdmaExchangeTracker::mark_node_ready(const std::string &node_id)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);

    // Check if node was registered
    if (registered_nodes_.count(node_id) == 0)
    {
        Logger::warning("RdmaExchangeTracker: Received RDMA_READY from unregistered node " + node_id);
        return false;
    }

    // Check if already marked ready
    if (ready_nodes_.count(node_id) > 0)
    {
        Logger::warning("RdmaExchangeTracker: Node " + node_id + " already marked ready");
        return all_nodes_ready();
    }

    // Mark as ready
    ready_nodes_.insert(node_id);

    Logger::info("RdmaExchangeTracker: Node " + node_id + " is RDMA ready (" +
                 std::to_string(ready_nodes_.size()) + "/" +
                 std::to_string(registered_nodes_.size()) + ")");

    // Check if all nodes ready
    bool all_ready = (ready_nodes_.size() == registered_nodes_.size());

    if (all_ready)
    {
        Logger::info("RdmaExchangeTracker: All nodes RDMA ready! Cluster operational.");
        cv_.notify_all();
    }

    return all_ready;
}

bool RdmaExchangeTracker::all_nodes_ready() const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return (ready_nodes_.size() == registered_nodes_.size()) &&
           (registered_nodes_.size() > 0);
}

bool RdmaExchangeTracker::wait_for_all_ready(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);

    bool result = cv_.wait_for(lock, timeout, [this]()
                               { return (ready_nodes_.size() == registered_nodes_.size()) &&
                                        (registered_nodes_.size() > 0); });

    if (!result)
    {
        Logger::warning("RdmaExchangeTracker: Timeout waiting for all nodes to be ready. " +
                        std::to_string(ready_nodes_.size()) + "/" +
                        std::to_string(registered_nodes_.size()) + " ready");
    }

    return result;
}

std::unordered_set<std::string> RdmaExchangeTracker::get_ready_nodes() const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return ready_nodes_;
}

// ============================================================================
// Statistics
// ============================================================================

RdmaExchangeTracker::Stats RdmaExchangeTracker::get_stats() const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);

    Stats stats;
    stats.expected_prefill_nodes = expected_prefill_nodes_;
    stats.expected_decode_nodes = expected_decode_nodes_;
    stats.registered_prefill_nodes = registered_prefill_.size();
    stats.registered_decode_nodes = registered_decode_.size();
    stats.ready_nodes = ready_nodes_.size();
    stats.topology_validated = (registered_prefill_.size() == expected_prefill_nodes_) &&
                               (registered_decode_.size() == expected_decode_nodes_);
    stats.cluster_operational = (ready_nodes_.size() == registered_nodes_.size()) &&
                                (registered_nodes_.size() > 0);

    return stats;
}
