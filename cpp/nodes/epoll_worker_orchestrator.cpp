#include "cpp/nodes/epoll_worker_orchestrator.h"
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
EpollWorkerOrchestrator::EpollWorkerOrchestrator(EventQueue &eventqueue, TCPServer &client_tcp_server, TCPServer &server_tcp_server, NodeRegistry &node_registry)
    : EpollWorkerBase(eventqueue, server_tcp_server),
      client_tcp_server_(client_tcp_server),
      node_registry_(node_registry)

{
    Logger::info("EpollWorkerOrchestrator initialized with client TCP server on port " + std::to_string(client_tcp_server_.get_fd()) +
                 " and server TCP server on port " + std::to_string(server_tcp_server_.get_fd()));
}
void EpollWorkerOrchestrator::start()
{
    if (running_)
    {
        Logger::warning("EpollWorkerOrchestrator already running");
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

    // Set server listen socket to non-blocking (CRITICAL for edge-triggered epoll)
    if (!set_nonblocking(server_tcp_server_.get_fd()))
    {
        Logger::error("Failed to set server listen socket non-blocking");
        close(epoll_fd_);
        epoll_fd_ = -1;
        return;
    }

    // Add server listening socket to epoll
    ev.events = EPOLLIN | EPOLLET; // Edge-triggered
    ev.data.fd = server_tcp_server_.get_fd();

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, server_tcp_server_.get_fd(), &ev) < 0)
    {
        Logger::error("epoll_ctl() failed for server listen socket: " + std::string(strerror(errno)));
        close(epoll_fd_);
        epoll_fd_ = -1;
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
    io_thread_ = std::thread(&EpollWorkerOrchestrator::run, this);

    Logger::info("EpollWorker started");
}

// ========== Main Event Loop ==========

void EpollWorkerOrchestrator::handle_client_accept()
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

void EpollWorkerOrchestrator::handle_server_accept()
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

        // We add the connection to the registry
        node_registry_.add_node_fd(new_conn->get_node_host(), new_conn->get_fd());

        int fd = new_conn->get_fd();
        Logger::info("EpollWorker: accepted new server connection fd=" + std::to_string(fd));

        // Add to epoll
        if (!add_connection(std::move(new_conn), SERVER_CONN))
        {
            Logger::error("EpollWorker: failed to add server connection fd=" + std::to_string(fd));
        }
    }
}
