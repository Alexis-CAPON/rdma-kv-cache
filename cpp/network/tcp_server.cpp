#include "cpp/network/tcp_server.h"
#include "cpp/common/utils.h"
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>

TCPServer::TCPServer() : sockfd_(-1)
{
    std::memset(&addr_, 0, sizeof(addr_));
}

TCPServer::~TCPServer()
{
    close();
}

bool TCPServer::bind(uint16_t port)
{
    // 1. Create socket
    sockfd_ = socket(AF_INET, SOCK_STREAM, 0); // AF_INET : IPv4 Internet protocols  ; Type of socket ; protocol
    if (sockfd_ < 0)
    {
        return false;
    }

    // 2. Set SO_REUSEADDR (allows quick restart without "address already in use")
    int opt = 1;
    if (setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        ::close(sockfd_);
        sockfd_ = -1;
        return false;
    }

    // 3. Setup address -> We use memset because of the sensitive data, we want to be sure it's initiated with 0
    // It ensure the bytes are at 0 here
    std::memset(&addr_, 0, sizeof(addr_));
    addr_.sin_family = AF_INET;
    addr_.sin_addr.s_addr = INADDR_ANY; // Listen on all interfaces
    addr_.sin_port = htons(port);

    // 4. Bind socket to address
    if (::bind(sockfd_, (struct sockaddr *)&addr_, sizeof(addr_)) < 0)
    {
        ::close(sockfd_);
        sockfd_ = -1;
        return false;
    }

    return true;
}

bool TCPServer::listen(int max_queued_connections)
{
    if (::listen(sockfd_, max_queued_connections) < 0)
    {
        return false;
    }
    return true;
}

std::unique_ptr<Connection> TCPServer::accept()
{
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    // Accept blocks until a client connects
    int client_fd = ::accept(sockfd_, (struct sockaddr *)&client_addr, &client_len);

    if (client_fd < 0)
    {
        return nullptr; // Error or interrupted
    }

    // Apply TCP optimizations to accepted connection
    Utils::optimize_tcp_socket(client_fd);

    // Wrap in Connection object
    return std::make_unique<Connection>(client_fd);
}

void TCPServer::close()
{
    if (sockfd_ >= 0)
    {
        ::close(sockfd_);
        sockfd_ = -1;
    }
}