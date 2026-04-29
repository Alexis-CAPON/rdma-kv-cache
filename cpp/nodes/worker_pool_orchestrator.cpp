#include "cpp/nodes/worker_pool_orchestrator.h"
#include "cpp/common/logger.h"
#include <thread>

WorkerPoolOrchestrator::WorkerPoolOrchestrator(
    uint32_t num_workers,
    EventQueue &event_queue,
    EpollWorkerOrchestrator &epoll_worker,
    const Config &config,
    const std::string &node_id,
    NodeRegistry &node_registry,
    RdmaExchangeTracker &rdma_exchange_tracker,
    RequestTrackerOrchestrator &request_tracker_orchestrator,
    RequestRouter &request_router)
    : WorkerPoolBase(num_workers, event_queue, config, node_id),
      epoll_worker_(epoll_worker),
      node_registry_(node_registry),
      rdma_exchange_tracker_(rdma_exchange_tracker),
      request_tracker_orchestrator_(request_tracker_orchestrator),
      request_router_(request_router)
{
    Logger::info("WorkerPool initializing with " + std::to_string(num_workers_) + " workers");

    // Pre-create Worker instances (one per thread)

    /*
    // Instance for Orchestrator
    WorkerOrchestrator(
        uint32_t worker_id,
        EpollWorkerOrchestrator &epoll_worker,
        const Config &config,
        uint64_t node_id,
        std::atomic<uint64_t> *server_ops_counter = nullptr,
        NodeRegistry &node_registry,
        RdmaExchangeTracker &rdma_exchange_tracker,
        RequestTrackerOrchestrator &request_tracker_orchestrator,
        RequestRouter &request_router);
*/

    workers_.reserve(num_workers_);
    for (uint32_t i = 0; i < num_workers_; i++)
    {
        workers_.push_back(std::make_unique<WorkerOrchestrator>(
            i, // worker_id
            epoll_worker_,
            config_,
            node_id_,
            &total_ops_,
            node_registry_,
            rdma_exchange_tracker_,
            request_tracker_orchestrator_,
            request_router_)); // Pass server-side operation counter
    }

    Logger::info("WorkerPoolOrchestrator initialized with " + std::to_string(workers_.size()) + " WorkerOrchestrator instances");
}

void WorkerPoolOrchestrator::start()
{
    if (running_.load())
    {
        Logger::warning("WorkerPoolOrchestrator::start() called but already running");
        return;
    }

    Logger::info("Starting WorkerPoolOrchestrator with " + std::to_string(num_workers_) + " threads");

    running_.store(true);

    // Launch worker threads
    threads_.reserve(num_workers_);
    for (uint32_t i = 0; i < num_workers_; i++)
    {
        threads_.emplace_back(&WorkerPoolOrchestrator::worker_thread_function, this, i);
    }

    Logger::info("WorkerPoolOrchestrator started successfully");
}

void WorkerPoolOrchestrator::stop()
{
    if (!running_.load())
    {
        return; // Already stopped
    }

    Logger::info("Stopping WorkerPoolOrchestrator...");

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

    Logger::info("WorkerPoolOrchestrator stopped (all threads joined)");
}

void WorkerPoolOrchestrator::worker_thread_function(uint32_t worker_id)
{
    Logger::info("Worker thread " + std::to_string(worker_id) + " started");

    WorkerOrchestrator *worker = workers_[worker_id].get();

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
