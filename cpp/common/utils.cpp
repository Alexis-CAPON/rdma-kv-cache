#include "utils.h"
#include "common/logger.h"
#include <thread>
#include <atomic>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>

// Request ID generator
uint64_t Utils::generate_request_id()
{
    static std::atomic<uint64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

uint64_t Utils::current_timestamp_us()
{
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
}

uint64_t Utils::current_timestamp_ms()
{
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

uint64_t Utils::hash_key(const std::string &key)
{
    // Simple FNV-1a hash
    uint64_t hash = 14695981039346656037ULL;
    for (char c : key)
    {
        hash ^= static_cast<uint8_t>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

void Utils::sleep_us(uint64_t us)
{
    std::this_thread::sleep_for(std::chrono::microseconds(us));
}

void Utils::sleep_ms(uint64_t ms)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// ============================================================================
// TCP Socket Optimization Functions
// ============================================================================

bool Utils::set_tcp_nodelay(int sockfd, bool enable)
{
    int flag = enable ? 1 : 0;
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0)
    {
        Logger::warning("Failed to set TCP_NODELAY on socket " + std::to_string(sockfd));
        return false;
    }
    Logger::debug("Set TCP_NODELAY=" + std::to_string(enable) + " on socket " + std::to_string(sockfd));
    return true;
}

bool Utils::set_tcp_keepalive(int sockfd, bool enable, int idle_secs, int interval_secs, int count)
{
    int flag = enable ? 1 : 0;

    // Enable/disable TCP keepalive
    if (setsockopt(sockfd, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag)) < 0)
    {
        Logger::warning("Failed to set SO_KEEPALIVE on socket " + std::to_string(sockfd));
        return false;
    }

    if (enable)
    {
        // Set keepalive parameters (platform-specific)
#ifdef __APPLE__
        // macOS uses TCP_KEEPALIVE instead of TCP_KEEPIDLE
        if (setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPALIVE, &idle_secs, sizeof(idle_secs)) < 0)
        {
            Logger::warning("Failed to set TCP_KEEPALIVE idle time on socket " + std::to_string(sockfd));
        }
#else
        // Linux
        if (setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPIDLE, &idle_secs, sizeof(idle_secs)) < 0)
        {
            Logger::warning("Failed to set TCP_KEEPIDLE on socket " + std::to_string(sockfd));
        }

        if (setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPINTVL, &interval_secs, sizeof(interval_secs)) < 0)
        {
            Logger::warning("Failed to set TCP_KEEPINTVL on socket " + std::to_string(sockfd));
        }

        if (setsockopt(sockfd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count)) < 0)
        {
            Logger::warning("Failed to set TCP_KEEPCNT on socket " + std::to_string(sockfd));
        }
#endif
        Logger::debug("Enabled TCP keepalive on socket " + std::to_string(sockfd) +
                     " (idle=" + std::to_string(idle_secs) + "s, interval=" + std::to_string(interval_secs) +
                     "s, count=" + std::to_string(count) + ")");
    }

    return true;
}

bool Utils::set_send_buffer_size(int sockfd, int size_bytes)
{
    if (setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &size_bytes, sizeof(size_bytes)) < 0)
    {
        Logger::warning("Failed to set SO_SNDBUF on socket " + std::to_string(sockfd));
        return false;
    }
    Logger::debug("Set send buffer size to " + std::to_string(size_bytes) + " bytes on socket " +
                 std::to_string(sockfd));
    return true;
}

bool Utils::set_recv_buffer_size(int sockfd, int size_bytes)
{
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVBUF, &size_bytes, sizeof(size_bytes)) < 0)
    {
        Logger::warning("Failed to set SO_RCVBUF on socket " + std::to_string(sockfd));
        return false;
    }
    Logger::debug("Set recv buffer size to " + std::to_string(size_bytes) + " bytes on socket " +
                 std::to_string(sockfd));
    return true;
}

bool Utils::optimize_tcp_socket(int sockfd)
{
    bool success = true;

    // 1. Disable Nagle's algorithm for low latency
    success &= set_tcp_nodelay(sockfd, true);

    // 2. Enable TCP keepalive to detect dead connections
    // idle=60s, probe every 10s, 3 probes before considering dead
    success &= set_tcp_keepalive(sockfd, true, 60, 10, 3);

    // 3. Increase send/receive buffer sizes for better throughput
    // 256KB buffers (reasonable for high-throughput connections)
    success &= set_send_buffer_size(sockfd, 256 * 1024);
    success &= set_recv_buffer_size(sockfd, 256 * 1024);

    if (success)
    {
        Logger::debug("Successfully optimized TCP socket " + std::to_string(sockfd));
    }
    else
    {
        Logger::warning("Some TCP optimizations failed for socket " + std::to_string(sockfd));
    }

    return success;
}