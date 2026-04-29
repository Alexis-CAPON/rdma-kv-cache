#pragma once
#include "cpp/common/logger.h"
#include "cpp/common/connection.h"
#include "cpp/common/messages.h"
#include <thread>
#include <atomic>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <chrono>
#include "cpp/nodes/epoll_worker_base.h"
#include "cpp/common/types.h"

/**
 * EpollWorkerNode - Unified epoll-based I/O handler
 *
 * Handles both client and server-to-server TCP connections
 * using edge-triggered epoll for maximum performance.
 *
 * Features:
 * - Edge-triggered epoll (EPOLLET)
 * - Non-blocking I/O
 * - Length-prefix framing (4-byte header)
 * - State machine for partial message handling
 * - Unified handling for client and server connections
 */
class EpollWorkerNode : public EpollWorkerBase
{
public:
    EpollWorkerNode(EventQueue &eventqueue, TCPServer &server_tcp_server, NodeInfo &node_info);

    int get_orchestrator_fd()
    {
        return orchestrator_fd_;
    };

protected:
    /**
     * Start the epoll I/O thread
     */
    void start() override;

    void handle_server_accept() override;

private:
    NodeInfo &node_info_;

    int orchestrator_fd_ = -1; // File descriptor for orchestrator connection (if connected)

    bool connect_to_orchestrator(const std::string &host, uint16_t port);

    bool send_node_info_to_orchestrator();
};
