#pragma once

#include <string>
#include <cstdint>
#include <chrono>
#include <functional>

// Utility functions
class Utils
{
public:
    // Generate unique request ID
    static uint64_t generate_request_id();

    // Get current timestamp in microseconds
    static uint64_t current_timestamp_us();

    // Get current timestamp in milliseconds
    static uint64_t current_timestamp_ms();

    // Hash function for keys (for consistent hashing)
    static uint64_t hash_key(const std::string &key);

    // Sleep for given microseconds
    static void sleep_us(uint64_t us);

    // Sleep for given milliseconds
    static void sleep_ms(uint64_t ms);

    // TCP socket optimization functions
    // Enable TCP_NODELAY to disable Nagle's algorithm (lower latency)
    static bool set_tcp_nodelay(int sockfd, bool enable = true);

    // Enable TCP keepalive to detect dead connections
    static bool set_tcp_keepalive(int sockfd, bool enable = true,
                                   int idle_secs = 60, int interval_secs = 10, int count = 3);

    // Set socket send buffer size
    static bool set_send_buffer_size(int sockfd, int size_bytes);

    // Set socket receive buffer size
    static bool set_recv_buffer_size(int sockfd, int size_bytes);

    // Apply all TCP optimizations for low-latency connections
    static bool optimize_tcp_socket(int sockfd);
};
