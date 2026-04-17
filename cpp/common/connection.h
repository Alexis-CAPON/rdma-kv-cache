#pragma once

#include <vector>
#include <cstdint>
#include <unistd.h>
#include "cpp/common/messages.h"

class Connection
{
private:
    int sockfd_;
    std::vector<uint8_t> read_buffer_;
    std::vector<uint8_t> write_buffer_;

    // Track which node this connection belongs to (for connection pool)
    std::string node_host_;
    uint16_t node_port_;

public:
    Connection();
    explicit Connection(int sockfd);
    ~Connection();

    // Make the class non-copyable and movable
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;
    Connection(Connection &&other) noexcept;
    Connection &operator=(Connection &&other) noexcept;

    // Connection management
    bool connect(const std::string &address, uint16_t port);
    bool connect_with_ip(const std::string &ip_address, uint16_t port); // Direct IP, no DNS
    void close();

    bool is_connected() const
    {
        return sockfd_ >= 0;
    }

    int get_fd() const
    {
        return sockfd_;
    }

    // Node tracking (for connection pool)
    void set_node_info(const std::string &host, uint16_t port)
    {
        node_host_ = host;
        node_port_ = port;
    }

    std::string get_node_host() const
    {
        return node_host_;
    }

    uint16_t get_node_port() const
    {
        return node_port_;
    }

    // Sending and receiving data
    bool send_message(const Message &message);
    std::unique_ptr<Message> receive_message(int timeout_ms = -1);

    // Raw send/receive
    bool send_bytes(const std::vector<uint8_t> &data);
    bool receive_bytes(std::vector<uint8_t> &buffer, size_t max_length, int timeout_ms = -1);
};