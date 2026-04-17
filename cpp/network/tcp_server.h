#pragma once
#include <cstdint>
#include <memory>
#include "cpp/common/connection.h"
#include <netinet/in.h>

class TCPServer
{
public:
    TCPServer(/* args */);
    ~TCPServer();

    bool bind(uint16_t port);
    bool listen(int backlog = 128);
    std::unique_ptr<Connection> accept();
    void close();

    int get_fd() const
    {
        return sockfd_;
    }

private:
    int sockfd_;
    struct sockaddr_in addr_;
};