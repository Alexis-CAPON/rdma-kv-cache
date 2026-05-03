#include "cpp/nodes/epoll_worker_node.h"
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include "cpp/common/types.h"

// Constructor for Orchestrator

// Constructor for Prefill/Decode nodes
EpollWorkerNode::EpollWorkerNode(EventQueue &eventqueue, TCPServer &server_tcp_server, NodeInfo &node_info)
    : EpollWorkerBase(eventqueue, server_tcp_server),
      node_info_(node_info)
{

    Logger::info("EpollWorker initialized with server TCP server on port " + std::to_string(server_tcp_server_.get_fd()));
}

void EpollWorkerNode::start()
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

    // Set server listen socket to non-blocking (CRITICAL for edge-triggered epoll)
    if (!set_nonblocking(server_tcp_server_.get_fd()))
    {
        Logger::error("Failed to set server listen socket non-blocking");
        close(epoll_fd_);
        epoll_fd_ = -1;
        return;
    }

    // Add server listening socket to epoll
    struct epoll_event ev{};
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
    io_thread_ = std::thread([this]() { run(); });

    Logger::info("EpollWorker started");
}

bool EpollWorkerNode::connect_to_orchestrator(const std::string &host, uint16_t port)
{
    // Create NEW client connection (separate from our listening socket)
    auto conn = std::make_unique<Connection>();

    // This creates a new socket() and connect()s to the peer
    if (!conn->connect(host, port))
    {
        Logger::error("EpollWorker: failed to connect to orchestrator " + host + ":" + std::to_string(port));
        return false;
    }

    int fd = conn->get_fd();
    orchestrator_fd_ = fd;

    node_info_.node_own_orchestrator_fd = orchestrator_fd_;

    Logger::info("EpollWorker: connected to orchestrator " + host + ":" + std::to_string(port) +
                 " (fd=" + std::to_string(orchestrator_fd_) + ")");

    // Add to epoll and connections map
    return add_connection(std::move(conn), SERVER_CONN);
}

bool EpollWorkerNode::send_node_info_to_orchestrator()
{
    if (orchestrator_fd_ < 0)
    {
        Logger::warning("EpollWorker: not connected to orchestrator, cannot send node info");
        return false;
    }

    Message msg = Message::create_rdma_process_registration(node_info_.node_id, node_info_);
    enqueue_response(orchestrator_fd_, msg);

    Logger::info("EpollWorker: sent node info to orchestrator");

    return true;
}

void EpollWorkerNode::handle_server_accept()
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

        // We add the connection to the NodeInfo
        // FIX: Protect vector access with mutex (race with connect_to_peer and worker reads)
        {
            std::lock_guard<std::mutex> lock(node_info_mutex_);
            node_info_.node_other_node_fd.push_back({new_conn->get_node_host(), new_conn->get_fd()});
        }
        Logger::info("EpollWorker: accepted new server connection from " + new_conn->get_node_host() + " fd=" + std::to_string(new_conn->get_fd()));

        int fd = new_conn->get_fd();
        Logger::info("EpollWorker: accepted new server connection fd=" + std::to_string(fd));

        // Add to epoll
        if (!add_connection(std::move(new_conn), SERVER_CONN))
        {
            Logger::error("EpollWorker: failed to add server connection fd=" + std::to_string(fd));
        }
    }
}

void EpollWorkerNode::on_peer_connected(const std::string &host, int fd)
{
    std::lock_guard<std::mutex> lock(node_info_mutex_);
    node_info_.node_other_node_fd.push_back({host, fd});
}