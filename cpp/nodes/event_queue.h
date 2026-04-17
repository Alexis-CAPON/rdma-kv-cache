#pragma once
#include "cpp/common/messages.h"
#include <queue>
#include <mutex>
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
    {
        capacity_ = size;
    }
    ~EventQueue();

    bool push(Event &&event);
    bool pop(Event &event);

    size_t size();

    bool is_empty();

private:
    size_t capacity_;
    std::queue<Event> queue_;
    std::mutex mutex_;
};