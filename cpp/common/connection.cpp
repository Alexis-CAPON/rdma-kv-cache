#include "common/connection.h"
#include "common/messages.h"
#include "common/utils.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cstring>
#include <cerrno>
#include <chrono>
#include <string>

Connection::Connection() : sockfd_(-1), node_port_(0)
{
    read_buffer_.reserve(65536);
    write_buffer_.reserve(65536);
}

Connection::Connection(int sockfd) : sockfd_(sockfd), node_port_(0)
{
    read_buffer_.reserve(65536);
    write_buffer_.reserve(65536);
}

Connection::~Connection()
{
    close();
}

// Move constructor
Connection::Connection(Connection &&other) noexcept
    : sockfd_(other.sockfd_),
      read_buffer_(std::move(other.read_buffer_)),
      write_buffer_(std::move(other.write_buffer_)),
      node_host_(std::move(other.node_host_)),
      node_port_(other.node_port_)
{
    other.sockfd_ = -1;
    other.node_port_ = 0;
}

// Move assignment
Connection &Connection::operator=(Connection &&other) noexcept
{
    if (this != &other)
    {
        close();

        sockfd_ = other.sockfd_;
        read_buffer_ = std::move(other.read_buffer_);
        write_buffer_ = std::move(other.write_buffer_);
        node_host_ = std::move(other.node_host_);
        node_port_ = other.node_port_;

        other.sockfd_ = -1;
        other.node_port_ = 0;
    }
    return *this;
}

// Connection management
bool Connection::connect(const std::string &address, uint16_t port)
{
    // 1. Create socket
    sockfd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd_ < 0)
    {
        return false;
    }

    // 2. Resolve hostname (thread-safe)
    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo *result = nullptr;
    const std::string port_str = std::to_string(port);
    int gai_rc = getaddrinfo(address.c_str(), port_str.c_str(), &hints, &result);
    if (gai_rc != 0 || result == nullptr)
    {
        ::close(sockfd_);
        sockfd_ = -1;
        return false;
    }

    // 3. Connect (blocking)
    const int connect_rc = ::connect(sockfd_, result->ai_addr, result->ai_addrlen);
    freeaddrinfo(result);

    if (connect_rc < 0)
    {
        ::close(sockfd_);
        sockfd_ = -1;
        return false;
    }

    // 4. Apply TCP optimizations (low latency + keepalive)
    Utils::optimize_tcp_socket(sockfd_);

    return true;
}

bool Connection::connect_with_ip(const std::string &ip_address, uint16_t port)
{
    // Fast path: connect directly with IP address, no DNS resolution

    // 1. Create socket
    sockfd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd_ < 0)
    {
        return false;
    }

    // 2. Convert IP string to binary form
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    // Convert IP address string to binary
    if (inet_pton(AF_INET, ip_address.c_str(), &addr.sin_addr) <= 0)
    {
        ::close(sockfd_);
        sockfd_ = -1;
        return false;
    }

    // 3. Connect (blocking)
    if (::connect(sockfd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        ::close(sockfd_);
        sockfd_ = -1;
        return false;
    }

    // 4. Apply TCP optimizations (low latency + keepalive)
    Utils::optimize_tcp_socket(sockfd_);

    return true;
}

void Connection::close()
{
    if (sockfd_ >= 0)
    {
        ::close(sockfd_);
        sockfd_ = -1;
    }
    read_buffer_.clear();
    write_buffer_.clear();
}

// Sending and receiving data
bool Connection::send_message(const Message &message)
{
    // 1. Serialize message to bytes
    std::vector<uint8_t> payload = serialize_message(message);

    // 2. Create length prefix (4 bytes, network byte order)
    uint32_t length = payload.size();
    uint32_t network_length = htonl(length);

    // 3. Build complete frame: [length][payload]
    write_buffer_.clear();
    write_buffer_.reserve(4 + payload.size());

    // Append length (4 bytes)
    uint8_t *length_bytes = reinterpret_cast<uint8_t *>(&network_length);
    write_buffer_.insert(write_buffer_.end(), length_bytes, length_bytes + 4);

    // Append payload
    write_buffer_.insert(write_buffer_.end(), payload.begin(), payload.end());

    // 4. Send all bytes
    return send_bytes(write_buffer_);
}

std::unique_ptr<Message> Connection::receive_message(int timeout_ms)
{
    // 1. Read 4-byte length prefix
    std::vector<uint8_t> length_buffer(4);
    if (!receive_bytes(length_buffer, 4, timeout_ms))
    {
        return nullptr;
    }

    // 2. Parse length (network to host byte order)
    uint32_t network_length;
    std::memcpy(&network_length, length_buffer.data(), 4);
    uint32_t payload_length = ntohl(network_length);

    // 3. Sanity check (prevent huge allocations)
    if (payload_length == 0 || payload_length > 10 * 1024 * 1024)
    { // 10 MB max
        return nullptr;
    }

    // 4. Read payload
    std::vector<uint8_t> payload(payload_length);
    if (!receive_bytes(payload, payload_length, timeout_ms))
    {
        return nullptr;
    }

    // 5. Deserialize payload to Message
    return deserialize_message(payload);
}

// Raw send/receive
bool Connection::send_bytes(const std::vector<uint8_t> &data)
{
    if (sockfd_ < 0)
    {
        return false;
    }

    size_t total_sent = 0;
    size_t remaining = data.size();

    while (remaining > 0)
    {
        ssize_t sent = ::send(sockfd_, data.data() + total_sent, remaining, 0);

        if (sent < 0)
        {
            if (errno == EINTR)
            {
                continue; // Interrupted by signal, retry
            }
            return false; // Error
        }

        if (sent == 0)
        {
            return false; // Connection closed
        }

        total_sent += sent;
        remaining -= sent;
    }

    return true;
}

bool Connection::receive_bytes(std::vector<uint8_t> &buffer, size_t length, int timeout_ms)
{
    if (sockfd_ < 0)
    {
        return false;
    }

    size_t total_received = 0;

    const auto start = std::chrono::steady_clock::now();

    while (total_received < length)
    {
        if (timeout_ms >= 0)
        {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - start)
                               .count();
            int remaining_timeout = timeout_ms - static_cast<int>(elapsed);
            if (remaining_timeout <= 0)
            {
                return false;
            }

            struct pollfd pfd;
            pfd.fd = sockfd_;
            pfd.events = POLLIN;
            pfd.revents = 0;

            int poll_result = ::poll(&pfd, 1, remaining_timeout);
            if (poll_result < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                return false;
            }

            if (poll_result == 0)
            {
                return false;
            }

            if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
            {
                return false;
            }
        }

        // 2. Receive data
        ssize_t received = ::recv(sockfd_,
                                  buffer.data() + total_received,
                                  length - total_received,
                                  0);

        if (received < 0)
        {
            if (errno == EINTR)
            {
                continue; // Interrupted, retry
            }
            return false; // Error
        }

        if (received == 0)
        {
            return false; // Connection closed by peer
        }

        total_received += received;
    }

    return true;
}
