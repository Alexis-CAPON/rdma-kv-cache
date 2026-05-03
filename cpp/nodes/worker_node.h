#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>
#include "cpp/nodes/event_queue.h"
#include "cpp/common/config.h"
#include "cpp/common/messages.h"
#include "cpp/common/types.h"
#include "cpp/nodes/epoll_worker_node.h"
#include "cpp/rdma/rdma_engine.h"
#include "cpp/nodes/request_tracker_layer.h"
#include "cpp/nodes/node.h"
#include "cpp/nodes/worker_base.h"
#include "cpp/nodes/worker_pool_base.h"

class WorkerNode : public WorkerBase
{
public:
    // Instance for prefill/decode nodes
    WorkerNode(
        uint32_t worker_id,
        EpollWorkerNode &epoll_worker,
        const Config &config,
        const std::string &node_id,
        std::atomic<uint64_t> *server_ops_counter,
        NodeInfo &node_info,
        RDMAEngine &rdma_engine,
        RequestTrackerLayer &request_tracker_layer,
        Node *node_ptr = nullptr);

    // Main entry point: process one event
    void process_event(Event &event) override;

private:
    EpollWorkerNode &epoll_worker_;

    NodeInfo &node_info_;                        // only used by prefill/decode workers
    RDMAEngine &rdma_engine_;                    // only used by prefill/decode workers
    RequestTrackerLayer &request_tracker_layer_; // only used by decode workers
    Node *node_ptr_;                             // Only used by prefill/decode workers (for vLLM server management)

    // ========================================
    // Event Handlers
    // ========================================

    // Both prefill and decode nodes
    void handle_broadcast_member_info(Message *msg);

    // Only prefill nodes
    void handle_assign_request(int server_fd, Message *msg);
    void handle_transfer_ready(int server_fd, Message *msg);

    // Only decode nodes
    void handle_prefill_complete(int server_fd, Message *msg);
    void handle_kv_transfer_complete(int server_fd, Message *msg);
    void handle_prepare_decode_slot(int server_fd, Message *msg);

    // ========================================
    // Response Handling
    // ========================================
    void send_client_response(int client_fd, const Message &response) override;
    void send_server_response(int server_fd, const Message &response) override;
};
