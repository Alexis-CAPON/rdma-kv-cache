#pragma once
#include "cpp/network/tcp_server.h"
#include "cpp/nodes/event_queue.h"
#include "cpp/common/logger.h"
#include "cpp/common/connection.h"
#include "cpp/common/messages.h"
#include <thread>
#include <atomic>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <sys/epoll.h>
#include <chrono>

/**
 * EpollWorker - Unified epoll-based I/O handler
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
class EpollWorkerBase
{
public:
    ~EpollWorkerBase();

    // Non-copyable
    EpollWorkerBase(const EpollWorkerBase &) = delete;
    EpollWorkerBase &operator=(const EpollWorkerBase &) = delete;

    /**
     * Stop the epoll I/O thread gracefully
     */
    void stop();

    /**
     * Enqueue response for sending (called by Workers)
     * Thread-safe
     */
    void enqueue_response(int fd, const Message &response);

    bool connect_to_peer(const std::string &host, uint16_t port);

protected:
    EpollWorkerBase(EventQueue &eventqueue, TCPServer &server_tcp_server);

    /**
     * Start the epoll I/O thread
     */
    // Configuration
    std::atomic<bool> running_;
    std::thread io_thread_;

    EventQueue &event_queue_;
    TCPServer &server_tcp_server_;

    // Socket types
    enum SocketType
    {
        LISTEN_CLIENT,
        LISTEN_SERVER,
        CLIENT_CONN,
        SERVER_CONN
    };

    // Per-connection state
    struct ConnectionState
    {
        SocketType type;
        std::unique_ptr<Connection> conn;

        // Input buffering (for receiving)
        std::vector<uint8_t> input_buffer;
        uint32_t expected_payload_length;
        bool reading_header;

        // Output buffering (for sending)
        std::vector<uint8_t> output_buffer;
        std::mutex output_mutex;

        // Server-specific fields (only used for SERVER_CONN)
        uint64_t peer_node_id;

        ConnectionState()
            : type(CLIENT_CONN),
              expected_payload_length(0),
              reading_header(true),
              peer_node_id(0)
        {
        }
    };

    // Epoll instance
    int epoll_fd_;

    // All active connections
    std::unordered_map<int, ConnectionState> connections_;
    std::mutex connections_mutex_;

    // Constants
    static constexpr uint32_t MAX_MESSAGE_SIZE = 16 * 1024 * 1024; // 16 MiB
    static constexpr int MAX_EPOLL_EVENTS = 1024;
    static constexpr size_t RECV_BUFFER_SIZE = 8192; // 8 KB

    virtual void start() = 0;

    // ========== Main Loop ==========

    /**
     * Main epoll event loop
     */
    void run();

    // ========== Accept Handlers ==========

    /**
     * Handle new client connection
     */
    // virtual void handle_client_accept() = 0;

    /**
     * Handle new server connection
     */
    virtual void handle_server_accept() = 0;

    // ========== I/O Handlers ==========

    /**
     * Handle read event (edge-triggered - reads until EAGAIN)
     */
    void handle_read(int fd);

    /**
     * Handle write event (edge-triggered - writes until EAGAIN)
     */
    void handle_write(int fd);

    /**
     * Handle error/hangup
     */
    void handle_error(int fd);

    // ========== Message Processing ==========

    /**
     * Parse complete messages from input buffer
     * Extracts all complete messages and pushes to event queue
     */
    void parse_messages(ConnectionState &state, int fd);

    // ========== Connection Management ==========

    /**
     * Add connection to epoll
     */
    bool add_connection(std::unique_ptr<Connection> conn, SocketType type);

    /**
     * Remove connection and cleanup
     */
    void remove_connection(int fd);

    /**
     * Modify epoll events for a socket
     */
    bool modify_epoll(int fd, uint32_t events);

    /**
     * Set socket to non-blocking mode
     */
    bool set_nonblocking(int fd);
};
