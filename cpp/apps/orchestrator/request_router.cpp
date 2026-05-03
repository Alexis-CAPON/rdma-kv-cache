#include "cpp/apps/orchestrator/request_router.h"
#include "cpp/common/logger.h"
#include <random>
#include <algorithm>
#include <mutex>

RequestRouter::RequestRouter(NodeRegistry &node_registry,
                            RoutingPolicy policy,
                            SlotConfig slot_config)
    : node_registry_(node_registry),
      policy_(policy),
      slot_config_(slot_config)
{
    Logger::info("RequestRouter: Initialized with policy=" + std::to_string(static_cast<int>(policy)) +
                 ", max_slots_per_node=" + std::to_string(slot_config_.max_slots_per_node) +
                 ", request_size=" + std::to_string(slot_config_.get_request_size_mb()) + "MB" +
                 ", num_layers=" + std::to_string(slot_config_.num_layers));
}

// ============================================================================
// PRIMARY METHOD: Select Nodes + Allocate Slot
// ============================================================================

std::optional<RequestRouter::RoutingResult>
RequestRouter::select_nodes_and_allocate_slot(const std::string& request_id)
{
    Logger::debug("RequestRouter: Selecting nodes and allocating slot for request " + request_id);

    // 1. Select prefill node
    auto prefill_node = select_prefill_node();
    if (!prefill_node) {
        Logger::warning("RequestRouter: No healthy prefill nodes available");
        return std::nullopt;
    }

    // 2. Select decode node WITH available slot
    auto decode_node = select_decode_node_with_slot();
    if (!decode_node) {
        Logger::warning("RequestRouter: No decode nodes with free slots available");
        return std::nullopt;
    }

    // 3. Allocate slot on selected decode node
    auto slot_id = allocate_slot_for_node(decode_node->node_id, request_id);
    if (!slot_id) {
        Logger::error("RequestRouter: Failed to allocate slot on " + decode_node->node_id +
                      " (race condition?)");
        return std::nullopt;
    }

    // 4. Calculate base offset for this slot
    size_t request_size_bytes = slot_config_.get_request_size_mb() * 1024UL * 1024UL;
    size_t slot_base_offset = slot_id.value() * request_size_bytes;

    // 5. Track request → decode node mapping (for freeing later)
    {
        std::unique_lock<std::shared_mutex> lock(slot_mutex_);
        request_to_decode_node_[request_id] = decode_node->node_id;
    }

    Logger::info("RequestRouter: Assigned request " + request_id +
                 " → prefill=" + prefill_node->node_id +
                 ", decode=" + decode_node->node_id +
                 ", slot=" + std::to_string(slot_id.value()) +
                 ", offset=0x" + std::to_string(slot_base_offset) +
                 " (" + std::to_string(slot_base_offset / (1024 * 1024)) + "MB)");

    return RoutingResult{
        .prefill_node = *prefill_node,
        .decode_node = *decode_node,
        .slot_id = slot_id.value(),
        .slot_base_offset = slot_base_offset
    };
}

void RequestRouter::free_slot(const std::string& request_id)
{
    std::unique_lock<std::shared_mutex> lock(slot_mutex_);

    // Find which decode node this request was assigned to
    auto it = request_to_decode_node_.find(request_id);
    if (it == request_to_decode_node_.end()) {
        Logger::warning("RequestRouter: Attempted to free slot for unknown request " + request_id);
        return;
    }

    std::string decode_node_id = it->second;
    request_to_decode_node_.erase(it);

    // Free the slot
    bool freed = free_slot_for_node(decode_node_id, request_id);
    if (freed) {
        Logger::info("RequestRouter: Freed slot for request " + request_id +
                     " on decode node " + decode_node_id);
    }
}

// ============================================================================
// Slot Management Helpers
// ============================================================================

void RequestRouter::initialize_node_slots(const std::string& decode_node_id)
{
    std::unique_lock<std::shared_mutex> lock(slot_mutex_);

    if (decode_node_slots_.find(decode_node_id) != decode_node_slots_.end()) {
        return;  // Already initialized
    }

    decode_node_slots_.emplace(
        decode_node_id,
        NodeSlotAllocator(decode_node_id, slot_config_.max_slots_per_node)
    );

    Logger::info("RequestRouter: Initialized " +
                 std::to_string(slot_config_.max_slots_per_node) +
                 " slots for decode node " + decode_node_id);
}

std::optional<int> RequestRouter::allocate_slot_for_node(
    const std::string& decode_node_id,
    const std::string& request_id)
{
    std::unique_lock<std::shared_mutex> lock(slot_mutex_);

    // Initialize slots if needed
    if (decode_node_slots_.find(decode_node_id) == decode_node_slots_.end()) {
        decode_node_slots_.emplace(
            decode_node_id,
            NodeSlotAllocator(decode_node_id, slot_config_.max_slots_per_node)
        );
    }

    NodeSlotAllocator& allocator = decode_node_slots_.at(decode_node_id);

    if (!allocator.has_free_slot()) {
        Logger::warning("RequestRouter: No free slots on node " + decode_node_id);
        return std::nullopt;
    }

    // Pop free slot
    int slot_id = allocator.free_slots.front();
    allocator.free_slots.pop();

    // Track mapping
    allocator.request_to_slot[request_id] = slot_id;
    allocator.slot_to_request[slot_id] = request_id;

    Logger::debug("RequestRouter: Allocated slot " + std::to_string(slot_id) +
                  " on node " + decode_node_id +
                  " for request " + request_id +
                  " (" + std::to_string(allocator.available_slots()) + "/" +
                  std::to_string(allocator.max_slots) + " slots remaining)");

    return slot_id;
}

bool RequestRouter::free_slot_for_node(
    const std::string& decode_node_id,
    const std::string& request_id)
{
    // Caller must hold slot_mutex_

    auto node_it = decode_node_slots_.find(decode_node_id);
    if (node_it == decode_node_slots_.end()) {
        Logger::warning("RequestRouter: Attempted to free slot on unknown node " + decode_node_id);
        return false;
    }

    NodeSlotAllocator& allocator = node_it->second;

    auto req_it = allocator.request_to_slot.find(request_id);
    if (req_it == allocator.request_to_slot.end()) {
        Logger::warning("RequestRouter: Request " + request_id + " has no slot on node " + decode_node_id);
        return false;
    }

    int slot_id = req_it->second;

    // Return slot to free pool
    allocator.free_slots.push(slot_id);
    allocator.request_to_slot.erase(req_it);
    allocator.slot_to_request.erase(slot_id);

    Logger::debug("RequestRouter: Freed slot " + std::to_string(slot_id) +
                  " on node " + decode_node_id +
                  " (" + std::to_string(allocator.available_slots()) + "/" +
                  std::to_string(allocator.max_slots) + " slots free)");

    return true;
}

// ============================================================================
// Select Decode Node WITH Available Slot
// ============================================================================

std::optional<NodeInfo> RequestRouter::select_decode_node_with_slot()
{
    auto healthy_decode_nodes = node_registry_.get_healthy_nodes(NodeRole::DECODE);

    if (healthy_decode_nodes.empty()) {
        Logger::warning("RequestRouter: No healthy decode nodes available");
        return std::nullopt;
    }

    // Filter nodes with available slots
    std::vector<NodeInfo> nodes_with_slots;

    {
        std::unique_lock<std::shared_mutex> lock(slot_mutex_);

        for (const auto& node : healthy_decode_nodes) {
            // Initialize if needed
            if (decode_node_slots_.find(node.node_id) == decode_node_slots_.end()) {
                decode_node_slots_.emplace(
                    node.node_id,
                    NodeSlotAllocator(node.node_id, slot_config_.max_slots_per_node)
                );
            }

            const auto& allocator = decode_node_slots_.at(node.node_id);
            if (allocator.has_free_slot()) {
                nodes_with_slots.push_back(node);
            }
        }
    }

    if (nodes_with_slots.empty()) {
        Logger::warning("RequestRouter: All decode nodes at capacity (no free slots)!");
        return std::nullopt;
    }

    // Select from nodes with free slots
    std::optional<NodeInfo> selected;

    switch (policy_) {
    case RoutingPolicy::ROUND_ROBIN:
        selected = select_round_robin(nodes_with_slots, decode_rr_index_);
        break;
    case RoutingPolicy::LEAST_LOADED:
        selected = select_least_loaded(nodes_with_slots);
        break;
    case RoutingPolicy::RANDOM:
        selected = select_random(nodes_with_slots);
        break;
    }

    if (selected) {
        Logger::debug("RequestRouter: Selected decode node " + selected->node_id + " (has free slots)");
    }

    return selected;
}

// ============================================================================
// Query Methods
// ============================================================================

int RequestRouter::get_available_slots(const std::string& decode_node_id) const
{
    std::shared_lock<std::shared_mutex> lock(slot_mutex_);

    auto it = decode_node_slots_.find(decode_node_id);
    if (it == decode_node_slots_.end()) {
        return slot_config_.max_slots_per_node;  // Not initialized = all free
    }

    return it->second.available_slots();
}

int RequestRouter::get_used_slots(const std::string& decode_node_id) const
{
    std::shared_lock<std::shared_mutex> lock(slot_mutex_);

    auto it = decode_node_slots_.find(decode_node_id);
    if (it == decode_node_slots_.end()) {
        return 0;
    }

    return it->second.used_slots();
}

// ============================================================================
// Legacy Node Selection Methods
// ============================================================================

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

// ============================================================================
// Load Tracking
// ============================================================================

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

// ============================================================================
// Helper Methods
// ============================================================================

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
