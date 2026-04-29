#include "cpp/nodes/worker_pool_node.h"
#include "cpp/common/logger.h"
#include <thread>

WorkerPoolNode::WorkerPoolNode(
    uint32_t num_workers,
    EventQueue &event_queue,
    EpollWorkerNode &epoll_worker,
    const Config &config,
    const std::string &node_id,
    NodeInfo &node_info,
    RDMAEngine &rdma_engine,
    RequestTrackerLayer &request_tracker_layer,
    Node *node_ptr)
    : WorkerPoolBase(num_workers, event_queue, config, node_id),
      epoll_worker_(epoll_worker),
      node_info_(node_info),
      rdma_engine_(rdma_engine),
      request_tracker_layer_(request_tracker_layer),
      node_ptr_(node_ptr)

{
    Logger::info("WorkerPoolNode initializing with " + std::to_string(num_workers_) + " workers");

    // Pre-create Worker instances (one per thread)
    workers_.reserve(num_workers_);
    for (uint32_t i = 0; i < num_workers_; i++)
    {
        workers_.push_back(std::make_unique<WorkerNode>(
            i, // worker_id
            epoll_worker_,
            config_,
            node_id_,
            &total_ops_,
            node_info_,
            rdma_engine_,
            request_tracker_layer_,
            node_ptr_)); // Pass server-side operation counter
    }

    Logger::info("WorkerPoolNode initialized with " + std::to_string(workers_.size()) + " WorkerNode instances");
}

void WorkerPoolNode::start()
{
    if (running_.load())
    {
        Logger::warning("WorkerPoolNode::start() called but already running");
        return;
    }

    Logger::info("Starting WorkerPoolNode with " + std::to_string(num_workers_) + " threads");

    running_.store(true);

    // Launch worker threads
    threads_.reserve(num_workers_);
    for (uint32_t i = 0; i < num_workers_; i++)
    {
        threads_.emplace_back(&WorkerPoolNode::worker_thread_function, this, i);
    }

    Logger::info("WorkerPoolNode started successfully");
}

void WorkerPoolNode::stop()
{
    if (!running_.load())
    {
        return; // Already stopped
    }

    Logger::info("Stopping WorkerPoolNode...");

    // Signal all threads to stop
    running_.store(false);

    // Wake any worker threads blocked in wait_and_pop
    event_queue_.wake_all();

    // Join all worker threads
    for (auto &thread : threads_)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }

    threads_.clear();

    Logger::info("WorkerPoolNode stopped (all threads joined)");
}

void WorkerPoolNode::worker_thread_function(uint32_t worker_id)
{
    Logger::info("Worker thread " + std::to_string(worker_id) + " started");

    WorkerNode *worker = workers_[worker_id].get();

    while (running_.load())
    {
        Event event;
        // Block until an event is available or the queue is stopped
        bool success = event_queue_.wait_and_pop(event);

        if (success)
        {
            worker->process_event(event);
        }
    }

    Logger::info("Worker thread " + std::to_string(worker_id) +
                 " stopped (" + std::to_string(worker->get_events_processed()) + " events processed)");
}
