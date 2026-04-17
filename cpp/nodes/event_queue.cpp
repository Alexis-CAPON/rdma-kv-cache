#include "cpp/nodes/event_queue.h"
#include "cpp/common/logger.h"

EventQueue::~EventQueue()
{
    Logger::info("EventQueue destroyed (had " + std::to_string(queue_.size()) + " events remaining)");
}

bool EventQueue::push(Event &&event)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // Check if queue is full
    if (queue_.size() >= capacity_)
    {
        Logger::warning("EventQueue is full (" + std::to_string(capacity_) + " events), dropping event");
        return false; // Queue full, cannot push
    }

    // Add event to queue
    queue_.push(std::move(event));

    return true;
}

bool EventQueue::pop(Event &event)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // Check if queue is empty
    if (queue_.empty())
    {
        return false; // Queue empty, no event available
    }

    // Get front event and move it out
    event = std::move(queue_.front());
    queue_.pop();

    return true;
}

size_t EventQueue::size()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

bool EventQueue::is_empty()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
}