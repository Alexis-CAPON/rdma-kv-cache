#pragma once

#include "cpp/apps/orchestrator/node_registry.h"
#include "cpp/nodes/epoll_worker_base.h"

/**
 * EpollWorkerOrchestrator - Unified epoll-based I/O handler for orchestrator
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
class EpollWorkerOrchestrator : public EpollWorkerBase
{
public:
    EpollWorkerOrchestrator(EventQueue &eventqueue, TCPServer &client_tcp_server, TCPServer &server_tcp_server, NodeRegistry &node_registry);

protected:
    void start() override;

    // ========== Accept Handlers ==========

    /**
     * Handle new server connection
     */
    void handle_server_accept() override;

private:
    TCPServer &client_tcp_server_;
    NodeRegistry &node_registry_;

    /**
     * Handle new client connection
     */
    void handle_client_accept();
};
