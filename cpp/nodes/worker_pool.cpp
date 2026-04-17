#include "cpp/nodes/worker_pool.h"
#include "cpp/common/logger.h"
#include <chrono>
#include <thread>

WorkerPool::WorkerPool(
    uint32_t num_workers,
    EventQueue &event_queue,
    EpollWorker &epoll_worker,
    const Config &config,
    uint64_t node_id)
    : num_workers_(num_workers),
      node_id_(node_id),
      running_(false),
      total_ops_(0),
      event_queue_(event_queue),
      epoll_worker_(epoll_worker),
      config_(config)
{
    Logger::info("WorkerPool initializing with " + std::to_string(num_workers_) + " workers");

    // Pre-create Worker instances (one per thread)
    workers_.reserve(num_workers_);
    for (uint32_t i = 0; i < num_workers_; i++)
    {
        workers_.push_back(std::make_unique<Worker>(
            i, // worker_id
            epoll_worker_,
            config_,
            node_id_,
            &total_ops_)); // Pass server-side operation counter
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
        // Try to pop an event from the queue
        Event event;
        bool success = event_queue_.pop(event);

        if (success)
        {
            // Process the event
            worker->process_event(event);
        }
        else
        {
            // Queue is empty, sleep briefly to avoid busy-waiting
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    Logger::info("Worker thread " + std::to_string(worker_id) +
                 " stopped (" + std::to_string(worker->get_events_processed()) + " events processed)");
}
