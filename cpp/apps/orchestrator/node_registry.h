#pragma once
#include "cpp/common/types.h"
#include <vector>
#include <unordered_map>
#include <shared_mutex>

class NodeRegistry
{
public:
    NodeRegistry() = default;

    // Node management
    void add_node(const NodeInfo &node);
    void remove_node(const std::string &node_id);
    void update_node_health(const std::string &node_id, bool is_healthy);

    int get_total_registered_nodes();

    // Node queries
    std::vector<NodeInfo> get_all_nodes() const;
    std::vector<NodeInfo> get_nodes_by_role(NodeRole role) const;
    std::vector<NodeInfo> get_healthy_nodes(NodeRole role) const;
    std::optional<NodeInfo> get_node(const std::string &node_id) const;

    // Statistics
    size_t get_node_count(NodeRole role) const;
    size_t get_healthy_node_count(NodeRole role) const;

    // Nodes FD
    void add_node_fd(const std::string &node_id, int fd);
    void remove_node_fd(const std::string &node_id);
    std::optional<int> get_node_fd(const std::string &node_id) const;

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, NodeInfo> nodes_;
    std::unordered_map<std::string, int> nodes_fd_;
};
