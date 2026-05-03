#include "cpp/nodes/worker_pool_base.h"
#include "cpp/common/logger.h"
#include <thread>

WorkerPoolBase::WorkerPoolBase(
    uint32_t num_workers,
    EventQueue &event_queue,
    const Config &config,
    const std::string &node_id)
    : num_workers_(num_workers),
      node_id_(node_id),
      running_(false),
      total_ops_(0),
      event_queue_(event_queue),
      config_(config)
{

    Logger::info("WorkerPoolBase initialized with " + std::to_string(num_workers_) + " Worker instances");
}

WorkerPoolBase::~WorkerPoolBase()
{
    // Note: derived classes are responsible for calling stop() in their own
    // destructors. Calling a pure virtual function here is undefined behavior.
}
