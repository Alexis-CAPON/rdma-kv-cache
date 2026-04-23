#include "cpp/apps/orchestrator/request_router.h"
#include "cpp/common/logger.h"
#include <random>
#include <algorithm>

RequestRouter::RequestRouter(NodeRegistry &node_registry, RoutingPolicy policy)
    : node_registry_(node_registry), policy_(policy)
{
    Logger::info("RequestRouter: Initialized with policy " + std::to_string(static_cast<int>(policy)));
}

std::optional<NodeInfo> RequestRouter::select_prefill_node()
{
    auto healthy_prefill_nodes = node_registry_.get_healthy_nodes(NodeRole::PREFILL);

    if (healthy_prefill_nodes.empty())
    {
        Logger::warning("RequestRouter: No healthy prefill nodes available");
        return std::nullopt;
    }

    std::optional<NodeInfo> selected;

    switch (policy_)
    {
    case RoutingPolicy::ROUND_ROBIN:
        selected = select_round_robin(healthy_prefill_nodes, prefill_rr_index_);
        break;
    case RoutingPolicy::LEAST_LOADED:
        selected = select_least_loaded(healthy_prefill_nodes);
        break;
    case RoutingPolicy::RANDOM:
        selected = select_random(healthy_prefill_nodes);
        break;
    }

    if (selected)
    {
        Logger::debug("RequestRouter: Selected prefill node " + selected->node_id);
    }

    return selected;
}

std::optional<NodeInfo> RequestRouter::select_decode_node()
{
    auto healthy_decode_nodes = node_registry_.get_healthy_nodes(NodeRole::DECODE);

    if (healthy_decode_nodes.empty())
    {
        Logger::warning("RequestRouter: No healthy decode nodes available");
        return std::nullopt;
    }

    std::optional<NodeInfo> selected;

    switch (policy_)
    {
    case RoutingPolicy::ROUND_ROBIN:
        selected = select_round_robin(healthy_decode_nodes, decode_rr_index_);
        break;
    case RoutingPolicy::LEAST_LOADED:
        selected = select_least_loaded(healthy_decode_nodes);
        break;
    case RoutingPolicy::RANDOM:
        selected = select_random(healthy_decode_nodes);
        break;
    }

    if (selected)
    {
        Logger::debug("RequestRouter: Selected decode node " + selected->node_id);
    }

    return selected;
}

std::optional<NodeInfo> RequestRouter::select_decode_node_for_prefill(const std::string &prefill_node_id)
{
    // For now, just select any healthy decode node
    // Future enhancement: implement affinity/locality
    // - Same rack/switch (for RDMA performance)
    // - Same NUMA node
    // - Previously used decode node for this prefill node
    auto selected = select_decode_node();

    if (selected)
    {
        Logger::debug("RequestRouter: Selected decode node " + selected->node_id +
                      " for prefill node " + prefill_node_id);
    }

    return selected;
}

void RequestRouter::increment_node_load(const std::string &node_id)
{
    std::unique_lock<std::shared_mutex> lock(load_mutex_);

    node_loads_[node_id]++;

    Logger::debug("RequestRouter: Incremented load for node " + node_id +
                  " to " + std::to_string(node_loads_[node_id]));
}

void RequestRouter::decrement_node_load(const std::string &node_id)
{
    std::unique_lock<std::shared_mutex> lock(load_mutex_);

    auto it = node_loads_.find(node_id);
    if (it == node_loads_.end())
    {
        Logger::warning("RequestRouter: Attempted to decrement load for unknown node " + node_id);
        return;
    }

    if (it->second > 0)
    {
        it->second--;
    }

    Logger::debug("RequestRouter: Decremented load for node " + node_id +
                  " to " + std::to_string(it->second));
}

int RequestRouter::get_node_load(const std::string &node_id) const
{
    std::shared_lock<std::shared_mutex> lock(load_mutex_);

    auto it = node_loads_.find(node_id);
    if (it == node_loads_.end())
    {
        return 0;
    }

    return it->second;
}

void RequestRouter::reset_node_load(const std::string &node_id)
{
    std::unique_lock<std::shared_mutex> lock(load_mutex_);

    node_loads_[node_id] = 0;

    Logger::info("RequestRouter: Reset load for node " + node_id);
}

void RequestRouter::set_policy(RoutingPolicy policy)
{
    policy_ = policy;
    Logger::info("RequestRouter: Changed policy to " + std::to_string(static_cast<int>(policy)));
}

RequestRouter::RoutingPolicy RequestRouter::get_policy() const
{
    return policy_;
}

// ========================================
// Helper Methods
// ========================================

std::optional<NodeInfo> RequestRouter::select_round_robin(const std::vector<NodeInfo> &nodes,
                                                           std::atomic<size_t> &index)
{
    if (nodes.empty())
    {
        return std::nullopt;
    }

    // Get next index and wrap around
    size_t current_index = index.fetch_add(1) % nodes.size();

    return nodes[current_index];
}

std::optional<NodeInfo> RequestRouter::select_least_loaded(const std::vector<NodeInfo> &nodes)
{
    if (nodes.empty())
    {
        return std::nullopt;
    }

    std::shared_lock<std::shared_mutex> lock(load_mutex_);

    // Find node with minimum load
    const NodeInfo *best_node = nullptr;
    int min_load = std::numeric_limits<int>::max();

    for (const auto &node : nodes)
    {
        int load = 0;
        auto it = node_loads_.find(node.node_id);
        if (it != node_loads_.end())
        {
            load = it->second;
        }

        if (load < min_load)
        {
            min_load = load;
            best_node = &node;
        }
    }

    if (best_node == nullptr)
    {
        return std::nullopt;
    }

    Logger::debug("RequestRouter: LEAST_LOADED selected node " + best_node->node_id +
                  " with load " + std::to_string(min_load));

    return *best_node;
}

std::optional<NodeInfo> RequestRouter::select_random(const std::vector<NodeInfo> &nodes)
{
    if (nodes.empty())
    {
        return std::nullopt;
    }

    // Use random_device for seeding
    static std::random_device rd;
    static std::mt19937 gen(rd());

    std::uniform_int_distribution<size_t> dist(0, nodes.size() - 1);
    size_t random_index = dist(gen);

    return nodes[random_index];
}
