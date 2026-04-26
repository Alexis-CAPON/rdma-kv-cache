#pragma once
#include "cpp/common/messages.h"
#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>

struct Event
{
    int client_fd;
    std::unique_ptr<Message> message;

    Event() : client_fd(-1), message(nullptr) {}
    Event(int fd, std::unique_ptr<Message> msg) : client_fd(fd), message(std::move(msg)) {}
};

class EventQueue
{
public:
    EventQueue(size_t size = 1024)
        : capacity_(size), stopped_(false)
    {
    }
    ~EventQueue();

    bool push(Event &&event);
    bool pop(Event &event);

    /**
     * Block until an event is available or the queue is stopped.
     * Returns true if an event was retrieved, false if stopped.
     */
    bool wait_and_pop(Event &event);

    /**
     * Wake all threads blocked in wait_and_pop (call before joining threads).
     */
    void wake_all();

    size_t size();

    bool is_empty();

private:
    size_t capacity_;
    bool stopped_;
    std::queue<Event> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
};