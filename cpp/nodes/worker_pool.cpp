#include "cpp/nodes/worker_pool.h"
#include "cpp/common/logger.h"
#include <thread>

WorkerPool::WorkerPool(
    uint32_t num_workers,
    EventQueue &event_queue,
    EpollWorker &epoll_worker,
    const Config &config,
    const std::string &node_id,
    NodeRegistry &node_registry,
    RdmaExchangeTracker &rdma_exchange_tracker,
    RequestTrackerOrchestrator &request_tracker_orchestrator,
    RequestRouter &request_router)
    : num_workers_(num_workers),
      node_id_(node_id),
      running_(false),
      total_ops_(0),
      event_queue_(event_queue),
      epoll_worker_(epoll_worker),
      config_(config),
      node_registry_(node_registry),
      rdma_exchange_tracker_(rdma_exchange_tracker),
      request_tracker_orchestrator_(request_tracker_orchestrator),
      request_router_(request_router)
{
    Logger::info("WorkerPool initializing with " + std::to_string(num_workers_) + " workers");

    // Pre-create Worker instances (one per thread)

    /*
    // Instance for Orchestrator
    Worker(
        uint32_t worker_id,
        EpollWorker &epoll_worker,
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
        workers_.push_back(std::make_unique<Worker>(
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

    Logger::info("WorkerPool initialized with " + std::to_string(workers_.size()) + " Worker instances");
}

WorkerPool::WorkerPool(
    uint32_t num_workers,
    EventQueue &event_queue,
    EpollWorker &epoll_worker,
    const Config &config,
    const std::string &node_id,
    NodeInfo &node_info,
    RDMAEngine &rdma_engine,
    Node *node_ptr)
    : num_workers_(num_workers),
      node_id_(node_id),
      running_(false),
      total_ops_(0),
      event_queue_(event_queue),
      epoll_worker_(epoll_worker),
      config_(config),
      node_info_(node_info),
      rdma_engine_(rdma_engine),
      node_ptr_(node_ptr)

{
    Logger::info("WorkerPool initializing with " + std::to_string(num_workers_) + " workers");

    // Pre-create Worker instances (one per thread)

    /*

        // Instance for prefill/decode nodes
    Worker(
        uint32_t worker_id,
        EpollWorker &epoll_worker,
        const Config &config,
        uint64_t node_id,
        std::atomic<uint64_t> *server_ops_counter = nullptr,
        NodeInfo &node_info,
        RDMAEngine &rdma_engine);
*/

    workers_.reserve(num_workers_);
    for (uint32_t i = 0; i < num_workers_; i++)
    {
        workers_.push_back(std::make_unique<Worker>(
            i, // worker_id
            epoll_worker_,
            config_,
            node_id_,
            &total_ops_,
            node_info_,
            rdma_engine_,
            node_ptr_)); // Pass server-side operation counter
    }

    Logger::info("WorkerPool initialized with " + std::to_string(workers_.size()) + " Worker instances");
}

WorkerPool::~WorkerPool()
{
    stop();
}

void WorkerPool::start()
{
    if (running_.load())
    {
        Logger::warning("WorkerPool::start() called but already running");
        return;
    }

    Logger::info("Starting WorkerPool with " + std::to_string(num_workers_) + " threads");

    running_.store(true);

    // Launch worker threads
    threads_.reserve(num_workers_);
    for (uint32_t i = 0; i < num_workers_; i++)
    {
        threads_.emplace_back(&WorkerPool::worker_thread_function, this, i);
    }

    Logger::info("WorkerPool started successfully");
}

void WorkerPool::stop()
{
    if (!running_.load())
    {
        return; // Already stopped
    }

    Logger::info("Stopping WorkerPool...");

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

    Logger::info("WorkerPool stopped (all threads joined)");
}

void WorkerPool::worker_thread_function(uint32_t worker_id)
{
    Logger::info("Worker thread " + std::to_string(worker_id) + " started");

    Worker *worker = workers_[worker_id].get();

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
