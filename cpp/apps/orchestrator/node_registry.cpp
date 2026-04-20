#include "cpp/apps/orchestrator/node_registry.h"
#include "cpp/common/logger.h"
#include <algorithm>

// ============================================================================
// Node Management
// ============================================================================

void NodeRegistry::add_node(const NodeInfo &node)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);

    nodes_[node.node_id] = node;

    Logger::info("NodeRegistry: Added node " + node.node_id +
                 " (role: " + (node.role == NodeRole::PREFILL ? "PREFILL" : "DECODE") +
                 ", qmaps: " + std::to_string(node.qmaps.size()) + ")");
}

void NodeRegistry::remove_node(const std::string &node_id)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);

    auto it = nodes_.find(node_id);
    if (it != nodes_.end())
    {
        Logger::info("NodeRegistry: Removing node " + node_id);
        nodes_.erase(it);
    }
    else
    {
        Logger::warning("NodeRegistry: Attempted to remove non-existent node " + node_id);
    }
}

void NodeRegistry::update_node_health(const std::string &node_id, bool is_healthy)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);

    auto it = nodes_.find(node_id);
    if (it != nodes_.end())
    {
        it->second.is_healthy = is_healthy;
        Logger::debug("NodeRegistry: Updated health for node " + node_id +
                      " to " + (is_healthy ? "healthy" : "unhealthy"));
    }
    else
    {
        Logger::warning("NodeRegistry: Attempted to update health for non-existent node " + node_id);
    }
}

// ============================================================================
// Node Queries
// ============================================================================

int NodeRegistry::get_total_registered_nodes()
{
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return nodes_.size();
}

std::vector<NodeInfo> NodeRegistry::get_all_nodes() const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);

    std::vector<NodeInfo> result;
    result.reserve(nodes_.size());

    for (const auto &[id, info] : nodes_)
    {
        result.push_back(info);
    }

    return result;
}

std::vector<NodeInfo> NodeRegistry::get_nodes_by_role(NodeRole role) const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);

    std::vector<NodeInfo> result;

    for (const auto &[id, info] : nodes_)
    {
        if (info.role == role)
        {
            result.push_back(info);
        }
    }

    return result;
}

std::vector<NodeInfo> NodeRegistry::get_healthy_nodes(NodeRole role) const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);

    std::vector<NodeInfo> result;

    for (const auto &[id, info] : nodes_)
    {
        if (info.role == role && info.is_healthy)
        {
            result.push_back(info);
        }
    }

    return result;
}

std::optional<NodeInfo> NodeRegistry::get_node(const std::string &node_id) const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);

    auto it = nodes_.find(node_id);
    if (it != nodes_.end())
    {
        return it->second;
    }

    return std::nullopt;
}

// ============================================================================
// Statistics
// ============================================================================

size_t NodeRegistry::get_node_count(NodeRole role) const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);

    size_t count = 0;
    for (const auto &[id, info] : nodes_)
    {
        if (info.role == role)
        {
            count++;
        }
    }

    return count;
}

size_t NodeRegistry::get_healthy_node_count(NodeRole role) const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);

    size_t count = 0;
    for (const auto &[id, info] : nodes_)
    {
        if (info.role == role && info.is_healthy)
        {
            count++;
        }
    }

    return count;
}

void NodeRegistry::add_node_fd(const std::string &node_id, int fd)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);
    nodes_fd_[node_id] = fd;
}

void NodeRegistry::remove_node_fd(const std::string &node_id)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);
    nodes_fd_.erase(node_id);
}

std::optional<int> NodeRegistry::get_node_fd(const std::string &node_id) const
{
    std::shared_lock<std::shared_mutex> lock(mutex_);

    auto it = nodes_fd_.find(node_id);
    if (it != nodes_fd_.end())
    {
        return it->second;
    }

    return std::nullopt;
}
