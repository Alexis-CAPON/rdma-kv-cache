#include "cpp/nodes/epoll_worker.h"
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

// Constructor for Orchestrator
EpollWorker::EpollWorker(EventQueue &eventqueue, TCPServer &client_tcp_server, TCPServer &server_tcp_server, NodeInfo &node_info, NodeRegistry &node_registry)
    : running_(false),
      event_queue_(eventqueue),
      client_tcp_server_(client_tcp_server),
      server_tcp_server_(server_tcp_server),
      epoll_fd_(-1),
      node_registry_(node_registry)

{
    Logger::info("EpollWorker initialized with client TCP server on port " + std::to_string(client_tcp_server_.get_fd()) +
                 " and server TCP server on port " + std::to_string(server_tcp_server_.get_fd()));
}

// Constructor for Prefill/Decode nodes
EpollWorker::EpollWorker(EventQueue &eventqueue, TCPServer &server_tcp_server, NodeInfo &node_info)
    : running_(false),
      event_queue_(eventqueue),
      client_tcp_server_(nullptr),
      server_tcp_server_(server_tcp_server),
      node_info_(node_info),
      epoll_fd_(-1)
{
    Logger::info("EpollWorker initialized with server TCP server on port " + std::to_string(server_tcp_server_.get_fd()));
}

EpollWorker::~EpollWorker()
{
    stop();

    if (epoll_fd_ >= 0)
    {
        close(epoll_fd_);
    }
}

void EpollWorker::start()
{
    if (running_)
    {
        Logger::warning("EpollWorker already running");
        return;
    }

    // Create epoll instance
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0)
    {
        Logger::error("epoll_create1() failed: " + std::string(strerror(errno)));
        return;
    }

    Logger::info("EpollWorker: created epoll fd=" + std::to_string(epoll_fd_));

    if (client_tcp_server_ != nullptr)
    {
        // Set client listen socket to non-blocking (CRITICAL for edge-triggered epoll)
        if (!set_nonblocking(client_tcp_server_.get_fd()))
        {
            Logger::error("Failed to set client listen socket non-blocking");
            close(epoll_fd_);
            return;
        }

        // Add client listening socket to epoll
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET; // Edge-triggered
        ev.data.fd = client_tcp_server_.get_fd();

        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_tcp_server_.get_fd(), &ev) < 0)
        {
            Logger::error("epoll_ctl() failed for client listen socket: " + std::string(strerror(errno)));
            close(epoll_fd_);
            return;
        }

        // Add to connections map (so we can identify it later)
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            ConnectionState &state = connections_[client_tcp_server_.get_fd()];
            state.type = LISTEN_CLIENT;
            state.reading_header = true;
        }

        Logger::info("EpollWorker: added client listen socket fd=" + std::to_string(client_tcp_server_.get_fd()));
    }

    // Set server listen socket to non-blocking (CRITICAL for edge-triggered epoll)
    if (!set_nonblocking(server_tcp_server_.get_fd()))
    {
        Logger::error("Failed to set server listen socket non-blocking");
        close(epoll_fd_);
        return;
    }

    // Add server listening socket to epoll
    ev.events = EPOLLIN | EPOLLET; // Edge-triggered
    ev.data.fd = server_tcp_server_.get_fd();

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, server_tcp_server_.get_fd(), &ev) < 0)
    {
        Logger::error("epoll_ctl() failed for server listen socket: " + std::string(strerror(errno)));
        close(epoll_fd_);
        return;
    }

    // Add to connections map
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        ConnectionState &state = connections_[server_tcp_server_.get_fd()];
        state.type = LISTEN_SERVER;
        state.reading_header = true;
    }

    Logger::info("EpollWorker: added server listen socket fd=" + std::to_string(server_tcp_server_.get_fd()));

    // Start I/O thread
    running_ = true;
    io_thread_ = std::thread(&EpollWorker::run, this);

    Logger::info("EpollWorker started");
}

void EpollWorker::stop()
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

void EpollWorker::run()
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

// ========== Accept Handlers ==========

void EpollWorker::handle_client_accept()
{
    // Edge-triggered: must accept all pending connections
    while (true)
    {
        auto new_conn = client_tcp_server_.accept();

        if (!new_conn)
        {
            // No more connections (EAGAIN) or error
            break;
        }

        int fd = new_conn->get_fd();
        Logger::info("EpollWorker: accepted new client fd=" + std::to_string(fd));

        // Add to epoll
        if (!add_connection(std::move(new_conn), CLIENT_CONN))
        {
            Logger::error("EpollWorker: failed to add client connection fd=" + std::to_string(fd));
        }
    }
}

void EpollWorker::handle_server_accept()
{
    // Edge-triggered: must accept all pending connections
    while (true)
    {
        auto new_conn = server_tcp_server_.accept();

        if (!new_conn)
        {
            // No more connections (EAGAIN) or error
            break;
        }

        if (node_info_.role != nullptr && (node_info_.role == NodeRole::PREFILL || node_info_.role == NodeRole::DECODE))
        {
            // We add the connection to the NodeInfo
            node_info_.node_other_node_fd.push_back({new_conn->get_node_host(), new_conn->get_fd()});
            Logger::info("EpollWorker: accepted new server connection from " + new_conn->get_node_host() + " fd=" + std::to_string(new_conn->get_fd()));
        }

        else
        {
            // We add the connection to the registry
            node_registry_.add_node_fd(new_conn->get_node_host(), new_conn->get_fd());
        }

        int fd = new_conn->get_fd();
        Logger::info("EpollWorker: accepted new server connection fd=" + std::to_string(fd));

        // Add to epoll
        if (!add_connection(std::move(new_conn), SERVER_CONN))
        {
            Logger::error("EpollWorker: failed to add server connection fd=" + std::to_string(fd));
        }
    }
}

// ========== I/O Handlers ==========

void EpollWorker::handle_read(int fd)
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

void EpollWorker::handle_write(int fd)
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

void EpollWorker::handle_error(int fd)
{
    Logger::info("EpollWorker: error on fd=" + std::to_string(fd));
    remove_connection(fd);
}

// ========== Message Processing ==========

void EpollWorker::parse_messages(ConnectionState &state, int fd)
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

bool EpollWorker::add_connection(std::unique_ptr<Connection> conn, SocketType type)
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

void EpollWorker::remove_connection(int fd)
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

bool EpollWorker::modify_epoll(int fd, uint32_t events)
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

bool EpollWorker::set_nonblocking(int fd)
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

void EpollWorker::enqueue_response(int client_fd, const Message &response)
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

bool EpollWorker::connect_to_orchestrator(const std::string &host, uint32_t port)
{
    // Create NEW client connection (separate from our listening socket)
    auto conn = std::make_unique<Connection>();

    // This creates a new socket() and connect()s to the peer
    if (!conn->connect(host, port))
    {
        Logger::error("EpollWorker: failed to connect to orchestrator " + host + ":" + std::to_string(port));
        return false;
    }

    int orchestrator_fd_ = conn->get_fd();

    node_info_.node_own_orchestrator_fd = orchestrator_fd_;

    Logger::info("EpollWorker: connected to orchestrator " + host + ":" + std::to_string(port) +
                 " (fd=" + std::to_string(orchestrator_fd_) + ")");

    // Add to epoll and connections map
    return add_connection(std::move(conn), SERVER_CONN);
}

bool EpollWorker::connect_to_peer(const std::string &host, uint32_t port)
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

    node_info_.node_own_peer_fd.push_back(peer_fd);

    Logger::info("EpollWorker: connected to peer " + host + ":" + std::to_string(port) +
                 " (fd=" + std::to_string(peer_fd) + ")");

    // Add to epoll and connections map
    return add_connection(std::move(conn), SERVER_CONN);
}

bool EpollWorker::send_node_info_to_orchestrator()
{
    if (orchestrator_fd_ < 0)
    {
        Logger::warning("EpollWorker: not connected to orchestrator, cannot send node info");
        return false;
    }

    Message msg = Message::create_rdma_process_registration(config_.node_id, node_info_);
    if (!enqueue_response(orchestrator_fd_, msg))
    {
        Logger::error("EpollWorker: failed to enqueue node info message to orchestrator");
        return false;
    }

    Logger::info("EpollWorker: sent node info to orchestrator");

    return true;
}
