#include "cpp/nodes/epoll_worker_base.h"
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include "cpp/common/types.h"

EpollWorkerBase::EpollWorkerBase(EventQueue &eventqueue, TCPServer &server_tcp_server)
    : running_(false),
      event_queue_(eventqueue),
      server_tcp_server_(server_tcp_server),
      epoll_fd_(-1)
{
    Logger::info("EpollWorkerBase initialized with server TCP server on port " + std::to_string(server_tcp_server_.get_fd()));
}

EpollWorkerBase::~EpollWorkerBase()
{
    stop();

    if (epoll_fd_ >= 0)
    {
        close(epoll_fd_);
    }
}

void EpollWorkerBase::stop()
{
    if (!running_)
    {
        return;
    }

    running_ = false;
    Logger::info("Stopping EpollWorker...");

    if (io_thread_.joinable())
    {
        io_thread_.join();
    }

    // Close all connections
    std::lock_guard<std::mutex> lock(connections_mutex_);
    for (auto &[fd, state] : connections_)
    {
        if (state.conn)
        {
            state.conn->close();
        }
    }
    connections_.clear();

    Logger::info("EpollWorker stopped");
}

// ========== Main Event Loop ==========

void EpollWorkerBase::run()
{
    struct epoll_event events[MAX_EPOLL_EVENTS];

    Logger::info("EpollWorker: entering event loop");

    while (running_)
    {
        // Wait for events (100ms timeout to check running_ flag periodically)
        int n = epoll_wait(epoll_fd_, events, MAX_EPOLL_EVENTS, 100);

        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue; // Interrupted by signal, retry
            }
            Logger::error("epoll_wait() failed: " + std::string(strerror(errno)));
            break;
        }

        if (n == 0)
        {
            // Timeout - check if still running
            continue;
        }

        Logger::debug("EpollWorker: epoll_wait returned " + std::to_string(n) + " events");

        // Process all events
        for (int i = 0; i < n; i++)
        {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;

            Logger::debug("EpollWorker: event on fd=" + std::to_string(fd) +
                          " events=0x" + std::to_string(ev));

            // Get socket type
            SocketType type;
            {
                std::lock_guard<std::mutex> lock(connections_mutex_);
                auto it = connections_.find(fd);
                if (it == connections_.end())
                {
                    Logger::warning("EpollWorker: event on unknown fd=" + std::to_string(fd));
                    continue;
                }
                type = it->second.type;
            }

            // Handle errors/hangup first
            if (ev & (EPOLLERR | EPOLLHUP))
            {
                Logger::debug("EpollWorker: error/hangup on fd=" + std::to_string(fd));
                handle_error(fd);
                continue;
            }

            // Dispatch based on socket type
            if (type == LISTEN_CLIENT)
            {
                if (ev & EPOLLIN)
                {
                    handle_client_accept();
                }
            }
            else if (type == LISTEN_SERVER)
            {
                if (ev & EPOLLIN)
                {
                    handle_server_accept();
                }
            }
            else // CLIENT_CONN or SERVER_CONN
            {
                if (ev & EPOLLIN)
                {
                    handle_read(fd);
                }
                if (ev & EPOLLOUT)
                {
                    handle_write(fd);
                }
            }
        }
    }

    Logger::info("EpollWorker: exited event loop");
}

// ========== I/O Handlers ==========

void EpollWorkerBase::handle_read(int fd)
{
    std::unique_lock<std::mutex> lock(connections_mutex_);

    auto it = connections_.find(fd);
    if (it == connections_.end())
    {
        Logger::warning("EpollWorker: handle_read on unknown fd=" + std::to_string(fd));
        return;
    }

    ConnectionState &state = it->second;

    // Edge-triggered: MUST read until EAGAIN
    while (true)
    {
        uint8_t temp_buffer[RECV_BUFFER_SIZE];
        ssize_t n = recv(fd, temp_buffer, sizeof(temp_buffer), 0);

        if (n > 0)
        {
            // Received data - append to buffer
            state.input_buffer.insert(state.input_buffer.end(),
                                      temp_buffer,
                                      temp_buffer + n);

            Logger::debug("EpollWorker: received " + std::to_string(n) +
                          " bytes from fd=" + std::to_string(fd) +
                          " (buffer now has " + std::to_string(state.input_buffer.size()) + " bytes)");
        }
        else if (n == 0)
        {
            // Connection closed by peer
            Logger::info("EpollWorker: connection closed by peer fd=" + std::to_string(fd));
            lock.unlock();
            remove_connection(fd);
            return;
        }
        else // n < 0
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                // No more data available (expected with edge-triggered)
                break;
            }
            else
            {
                // Real error
                Logger::error("EpollWorker: recv() error on fd=" + std::to_string(fd) +
                              ": " + std::string(strerror(errno)));
                lock.unlock();
                remove_connection(fd);
                return;
            }
        }
    }

    // Unlock BEFORE calling parse_messages to avoid deadlock
    // (parse_messages may call remove_connection which locks connections_mutex_)
    lock.unlock();

    // Parse complete messages from buffer
    parse_messages(state, fd);
}

void EpollWorkerBase::handle_write(int fd)
{
    std::unique_lock<std::mutex> lock(connections_mutex_);

    auto it = connections_.find(fd);
    if (it == connections_.end())
    {
        Logger::warning("EpollWorker: handle_write on unknown fd=" + std::to_string(fd));
        return;
    }

    ConnectionState &state = it->second;

    // Get data to send
    std::vector<uint8_t> data_to_send;
    {
        std::lock_guard<std::mutex> output_lock(state.output_mutex);
        if (state.output_buffer.empty())
        {
            // Nothing to send - stop monitoring EPOLLOUT
            lock.unlock();
            modify_epoll(fd, EPOLLIN);
            return;
        }
        data_to_send = std::move(state.output_buffer);
        state.output_buffer.clear();
    }

    // Edge-triggered: try to send all data
    size_t total_sent = 0;
    while (total_sent < data_to_send.size())
    {
        ssize_t sent = send(fd,
                            data_to_send.data() + total_sent,
                            data_to_send.size() - total_sent,
                            MSG_DONTWAIT);

        if (sent > 0)
        {
            total_sent += sent;
            Logger::debug("EpollWorker: sent " + std::to_string(sent) +
                          " bytes to fd=" + std::to_string(fd));
        }
        else if (sent < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                // Can't send more now - buffer the rest
                Logger::debug("EpollWorker: socket buffer full for fd=" + std::to_string(fd) +
                              ", buffering " + std::to_string(data_to_send.size() - total_sent) + " bytes");

                std::lock_guard<std::mutex> output_lock(state.output_mutex);
                state.output_buffer.insert(
                    state.output_buffer.begin(),
                    data_to_send.begin() + total_sent,
                    data_to_send.end());

                // Keep monitoring EPOLLOUT
                return;
            }
            else
            {
                // Error
                Logger::error("EpollWorker: send() error on fd=" + std::to_string(fd) +
                              ": " + std::string(strerror(errno)));
                lock.unlock();
                remove_connection(fd);
                return;
            }
        }
    }

    // All data sent successfully
    Logger::debug("EpollWorker: sent all " + std::to_string(total_sent) +
                  " bytes to fd=" + std::to_string(fd));

    // Check if there's more data queued while we were sending
    bool has_more;
    {
        std::lock_guard<std::mutex> output_lock(state.output_mutex);
        has_more = !state.output_buffer.empty();
    }

    if (!has_more)
    {
        // No more data - stop monitoring EPOLLOUT
        lock.unlock();
        modify_epoll(fd, EPOLLIN);
    }
}

void EpollWorkerBase::handle_error(int fd)
{
    Logger::info("EpollWorker: error on fd=" + std::to_string(fd));
    remove_connection(fd);
}

// ========== Message Processing ==========

void EpollWorkerBase::parse_messages(ConnectionState &state, int fd)
{
    // Try to extract complete messages from buffer
    while (true)
    {
        if (state.reading_header)
        {
            // PHASE 1: Reading 4-byte length header

            if (state.input_buffer.size() < 4)
            {
                // Not enough bytes yet for header
                Logger::debug("EpollWorker: waiting for header on fd=" + std::to_string(fd) +
                              " (have " + std::to_string(state.input_buffer.size()) + "/4 bytes)");
                return;
            }

            // Extract length (network byte order)
            uint32_t network_length;
            std::memcpy(&network_length, state.input_buffer.data(), 4);
            uint32_t length = ntohl(network_length);

            Logger::debug("EpollWorker: read header on fd=" + std::to_string(fd) +
                          ", expecting payload of " + std::to_string(length) + " bytes");

            // Sanity check
            if (length == 0 || length > MAX_MESSAGE_SIZE)
            {
                Logger::error("EpollWorker: invalid payload length " + std::to_string(length) +
                              " from fd=" + std::to_string(fd));
                remove_connection(fd);
                return;
            }

            // Save expected length and switch state
            state.expected_payload_length = length;
            state.reading_header = false;

            // Remove header from buffer
            state.input_buffer.erase(state.input_buffer.begin(),
                                     state.input_buffer.begin() + 4);
        }

        // PHASE 2: Reading payload

        if (state.input_buffer.size() < state.expected_payload_length)
        {
            // Not enough bytes yet for complete payload
            Logger::debug("EpollWorker: waiting for payload on fd=" + std::to_string(fd) +
                          " (have " + std::to_string(state.input_buffer.size()) + "/" +
                          std::to_string(state.expected_payload_length) + " bytes)");
            return;
        }

        // We have a complete message!
        std::vector<uint8_t> payload(state.input_buffer.begin(),
                                     state.input_buffer.begin() + state.expected_payload_length);

        Logger::debug("EpollWorker: complete message received on fd=" + std::to_string(fd) +
                      ", " + std::to_string(payload.size()) + " bytes");

        // Remove payload from buffer
        state.input_buffer.erase(state.input_buffer.begin(),
                                 state.input_buffer.begin() + state.expected_payload_length);

        // Reset state for next message
        state.reading_header = true;
        state.expected_payload_length = 0;

        // Deserialize message
        auto message = deserialize_message(payload);
        if (!message)
        {
            Logger::error("EpollWorker: failed to deserialize message from fd=" + std::to_string(fd));
            continue; // Try next message in buffer
        }

        // Push ALL messages to event queue (unified approach)
        // Workers will handle both client requests and server messages
        Event event(fd, std::move(message));

        // Client request → client queue
        if (!event_queue_.push(std::move(event)))
        {
            Logger::warning("EpollWorker: client event queue full, dropping message from fd=" + std::to_string(fd));
        }

        // Loop back - there might be more complete messages in buffer
    }
}

// ========== Connection Management ==========

bool EpollWorkerBase::add_connection(std::unique_ptr<Connection> conn, SocketType type)
{
    int fd = conn->get_fd();

    // Set non-blocking
    if (!set_nonblocking(fd))
    {
        Logger::error("EpollWorker: failed to set non-blocking for fd=" + std::to_string(fd));
        return false;
    }

    // Add to epoll with level-triggered mode
    // Level-triggered = notified as long as data is available to read
    struct epoll_event ev;
    ev.events = EPOLLIN; // Level-triggered (no EPOLLET)
    ev.data.fd = fd;

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0)
    {
        Logger::error("EpollWorker: epoll_ctl ADD failed for fd=" + std::to_string(fd) +
                      ": " + std::string(strerror(errno)));
        return false;
    }

    // Add to connections map
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        ConnectionState &state = connections_[fd];
        state.type = type;
        state.conn = std::move(conn);
        state.reading_header = true;
        state.expected_payload_length = 0;
        state.input_buffer.clear();
        state.output_buffer.clear();
    }

    Logger::debug("EpollWorker: added connection fd=" + std::to_string(fd) +
                  " type=" + std::to_string(type) + " (level-triggered)");

    return true;
}

void EpollWorkerBase::remove_connection(int fd)
{
    Logger::info("EpollWorker: removing connection fd=" + std::to_string(fd));

    // Remove from epoll
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);

    // Remove from connections map
    std::lock_guard<std::mutex> lock(connections_mutex_);
    auto it = connections_.find(fd);
    if (it != connections_.end())
    {
        if (it->second.conn)
        {
            it->second.conn->close();
        }
        connections_.erase(it);
    }

    Logger::debug("EpollWorker: removed connection fd=" + std::to_string(fd));
}

bool EpollWorkerBase::modify_epoll(int fd, uint32_t events)
{
    struct epoll_event ev;
    ev.events = events;
    ev.data.fd = fd;

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) < 0)
    {
        Logger::error("EpollWorker: epoll_ctl MOD failed for fd=" + std::to_string(fd) +
                      ": " + std::string(strerror(errno)));
        return false;
    }

    return true;
}

bool EpollWorkerBase::set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
    {
        Logger::error("EpollWorker: fcntl F_GETFL failed: " + std::string(strerror(errno)));
        return false;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
    {
        Logger::error("EpollWorker: fcntl F_SETFL O_NONBLOCK failed: " + std::string(strerror(errno)));
        return false;
    }

    return true;
}

// ========== Public Interface ==========

void EpollWorkerBase::enqueue_response(int client_fd, const Message &response)
{
    // Serialize message with framing
    std::vector<uint8_t> payload = serialize_message(response);

    uint32_t length = payload.size();
    uint32_t network_length = htonl(length);

    std::vector<uint8_t> serialized;
    serialized.reserve(4 + payload.size());

    // Add 4-byte length header
    uint8_t *length_bytes = reinterpret_cast<uint8_t *>(&network_length);
    serialized.insert(serialized.end(), length_bytes, length_bytes + 4);

    // Add payload
    serialized.insert(serialized.end(), payload.begin(), payload.end());

    // Add to connection's output buffer
    std::lock_guard<std::mutex> lock(connections_mutex_);

    auto it = connections_.find(client_fd);
    if (it == connections_.end())
    {
        Logger::warning("EpollWorker::enqueue_response: client_fd=" + std::to_string(client_fd) + " not found");
        return;
    }

    bool was_empty;
    {
        std::lock_guard<std::mutex> output_lock(it->second.output_mutex);
        was_empty = it->second.output_buffer.empty();
        it->second.output_buffer.insert(
            it->second.output_buffer.end(),
            serialized.begin(),
            serialized.end());
    }

    // If buffer was empty, start monitoring EPOLLOUT
    if (was_empty)
    {
        modify_epoll(client_fd, EPOLLIN | EPOLLOUT);
    }

    Logger::debug("EpollWorker::enqueue_response: queued " + std::to_string(serialized.size()) +
                  " bytes for client_fd=" + std::to_string(client_fd));
}

bool EpollWorkerBase::connect_to_peer(const std::string &host, uint16_t port)
{
    // Create NEW client connection (separate from our listening socket)
    auto conn = std::make_unique<Connection>();

    // This creates a new socket() and connect()s to the peer
    if (!conn->connect(host, port))
    {
        Logger::error("EpollWorker: failed to connect to peer " + host + ":" + std::to_string(port));
        return false;
    }

    int peer_fd = conn->get_fd();

    // FIX: Protect vector access with mutex (race with handle_server_accept and worker reads)
    {
        std::lock_guard<std::mutex> lock(node_info_mutex_);
        node_info_.node_other_node_fd.push_back({host, peer_fd});
    }

    Logger::info("EpollWorker: connected to peer " + host + ":" + std::to_string(port) +
                 " (fd=" + std::to_string(peer_fd) + ")");

    // Add to epoll and connections map
    return add_connection(std::move(conn), SERVER_CONN);
}