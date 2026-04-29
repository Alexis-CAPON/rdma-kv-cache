#pragma once

#include <cstdint>
#include <vector>
#include <thread>
#include <atomic>
#include <memory>
#include "cpp/nodes/event_queue.h"
#include "cpp/nodes/worker_node.h"
#include "cpp/nodes/epoll_worker_node.h"
#include "cpp/common/config.h"
#include "cpp/nodes/request_tracker_layer.h"
#include "cpp/nodes/node.h"

#include "cpp/nodes/worker_pool_base.h"

#include "cpp/rdma/rdma_engine.h"
#include "cpp/common/types.h"

class WorkerPoolNode : public WorkerPoolBase
{
public:
    // Constructor for prefill/decode nodes
    WorkerPoolNode(
        uint32_t num_workers,
        EventQueue &event_queue,
        EpollWorkerNode &epoll_worker,
        const Config &config,
        const std::string &node_id,
        NodeInfo &node_info,
        RDMAEngine &rdma_engine,
        RequestTrackerLayer &request_tracker_layer,
        Node *node_ptr);

    // Start all worker threads
    void start() override;

    // Stop all worker threads gracefully
    void stop() override;

private:
    NodeInfo &node_info_;                        // only used by prefill/decode workers
    RDMAEngine &rdma_engine_;                    // only used by prefill/decode workers
    RequestTrackerLayer &request_tracker_layer_; // only used by decode workers
    Node *node_ptr_;                             // only used by prefill/decode workers (for vLLM server management)

    EpollWorkerNode &epoll_worker_;

    // Worker instances (one per thread)
    std::vector<std::unique_ptr<WorkerNode>> workers_;

    // Thread function: polls queue and delegates to Worker
    void worker_thread_function(uint32_t worker_id) override;
};
