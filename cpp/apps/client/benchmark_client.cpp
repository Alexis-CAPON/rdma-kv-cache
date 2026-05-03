// cpp/apps/benchmark_client.cpp
// Benchmark client for disaggregated LLM inference system

#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "cpp/common/messages.h"
#include "cpp/common/logger.h"

class BenchmarkClient
{
public:
    BenchmarkClient(const std::string &host, int port)
        : host_(host), port_(port), sock_fd_(-1), connected_(false)
    {
    }

    ~BenchmarkClient()
    {
        disconnect();
    }

    // Connect to orchestrator
    bool connect()
    {
        if (connected_)
        {
            Logger::warning("Already connected");
            return true;
        }

        Logger::info("Connecting to orchestrator at " + host_ + ":" + std::to_string(port_));

        // Create socket
        sock_fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sock_fd_ < 0)
        {
            Logger::error("Failed to create socket: " + std::string(strerror(errno)));
            return false;
        }

        // Set socket options
        int opt = 1;
        setsockopt(sock_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        // Set socket timeout (60 seconds for long-running requests)
        struct timeval timeout;
        timeout.tv_sec = 60;
        timeout.tv_usec = 0;
        setsockopt(sock_fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(sock_fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        // Resolve hostname
        struct addrinfo hints, *result;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        int ret = getaddrinfo(host_.c_str(), std::to_string(port_).c_str(), &hints, &result);
        if (ret != 0)
        {
            Logger::error("Failed to resolve hostname: " + std::string(gai_strerror(ret)));
            close(sock_fd_);
            sock_fd_ = -1;
            return false;
        }

        // Connect
        ret = ::connect(sock_fd_, result->ai_addr, result->ai_addrlen);
        freeaddrinfo(result);

        if (ret < 0)
        {
            Logger::error("Failed to connect: " + std::string(strerror(errno)));
            close(sock_fd_);
            sock_fd_ = -1;
            return false;
        }

        connected_ = true;
        Logger::info("Connected to orchestrator");
        return true;
    }

    // Disconnect from orchestrator
    void disconnect()
    {
        if (sock_fd_ >= 0)
        {
            close(sock_fd_);
            sock_fd_ = -1;
        }
        connected_ = false;
    }

    // Send a single request and wait for response (reuses existing TCP connection)
    bool send_request(const std::string &prompt, int max_tokens, std::string &response_text, double &latency_ms)
    {
        if (!connected_)
        {
            Logger::error("Not connected to orchestrator");
            return false;
        }

        auto start_time = std::chrono::high_resolution_clock::now();

        // Create request message
        Message request;
        request.type = MessageType::CLIENT_REQUEST;
        request.source_node_id = ""; // Client has no node ID
        request.request_info.request_id = generate_request_id();
        request.request_info.prompt = prompt;
        request.request_info.max_tokens = max_tokens;
        request.request_info.timestamp_created = std::chrono::system_clock::now().time_since_epoch().count();

        Logger::debug("Sending request: request_id=" + request.request_info.request_id +
                      ", prompt_len=" + std::to_string(prompt.length()) +
                      ", max_tokens=" + std::to_string(max_tokens));

        // Serialize and send over existing TCP connection
        std::vector<uint8_t> buffer = serialize_message(request);
        ssize_t sent = ::send(sock_fd_, buffer.data(), buffer.size(), 0);
        if (sent < 0)
        {
            Logger::error("Failed to send request: " + std::string(strerror(errno)));
            connected_ = false;
            return false;
        }

        if (sent != static_cast<ssize_t>(buffer.size()))
        {
            Logger::error("Partial send: " + std::to_string(sent) + "/" + std::to_string(buffer.size()));
            connected_ = false;
            return false;
        }

        // Wait for response on same TCP connection
        static constexpr size_t MAX_RECV_BUFFER = 65536;
        std::vector<uint8_t> recv_buffer(MAX_RECV_BUFFER);
        ssize_t received = ::recv(sock_fd_, recv_buffer.data(), recv_buffer.size(), 0);

        auto end_time = std::chrono::high_resolution_clock::now();
        latency_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();

        if (received < 0)
        {
            Logger::error("Failed to receive response: " + std::string(strerror(errno)));
            connected_ = false;
            return false;
        }

        if (received == 0)
        {
            Logger::error("Connection closed by orchestrator");
            connected_ = false;
            return false;
        }

        // Deserialize response
        Message response;
        try
        {
            recv_buffer.resize(received);
            auto msg_ptr = deserialize_message(recv_buffer);
            if (!msg_ptr)
            {
                Logger::error("Failed to deserialize response: null message");
                return false;
            }
            response = *msg_ptr;
        }
        catch (const std::exception &e)
        {
            Logger::error("Failed to deserialize response: " + std::string(e.what()));
            return false;
        }

        // Check response type
        if (response.type == MessageType::CLIENT_ERROR)
        {
            Logger::error("Received error from orchestrator: " + response.error_message);
            return false;
        }

        if (response.type != MessageType::CLIENT_RESPONSE)
        {
            Logger::error("Unexpected response type: " + std::to_string(static_cast<int>(response.type)));
            return false;
        }

        response_text = response.response_text;
        Logger::debug("Received response: length=" + std::to_string(response_text.length()) +
                      ", latency=" + std::to_string(latency_ms) + "ms");

        return true;
    }

    bool is_connected() const { return connected_; }

private:
    std::string host_;
    int port_;
    int sock_fd_;
    bool connected_;

    // Generate unique request ID
    std::string generate_request_id()
    {
        static std::atomic<uint64_t> counter{0};
        auto now = std::chrono::system_clock::now().time_since_epoch().count();
        return "req_" + std::to_string(now) + "_" + std::to_string(counter++);
    }
};

// Statistics tracker
struct BenchmarkStats
{
    std::atomic<uint64_t> total_requests{0};
    std::atomic<uint64_t> successful_requests{0};
    std::atomic<uint64_t> failed_requests{0};
    std::atomic<uint64_t> total_latency_ms{0};
    std::atomic<uint64_t> min_latency_ms{UINT64_MAX};
    std::atomic<uint64_t> max_latency_ms{0};

    void add_result(bool success, double latency_ms)
    {
        total_requests++;
        if (success)
        {
            successful_requests++;
            uint64_t lat = static_cast<uint64_t>(latency_ms);
            total_latency_ms += lat;

            // Update min/max (not perfectly thread-safe but good enough for stats)
            uint64_t current_min = min_latency_ms.load();
            while (lat < current_min && !min_latency_ms.compare_exchange_weak(current_min, lat))
                ;

            uint64_t current_max = max_latency_ms.load();
            while (lat > current_max && !max_latency_ms.compare_exchange_weak(current_max, lat))
                ;
        }
        else
        {
            failed_requests++;
        }
    }

    void print_summary()
    {
        std::cout << "\n";
        std::cout << "========================================\n";
        std::cout << "Benchmark Results\n";
        std::cout << "========================================\n";
        std::cout << "Total requests:      " << total_requests << "\n";
        std::cout << "Successful:          " << successful_requests << "\n";
        std::cout << "Failed:              " << failed_requests << "\n";

        if (successful_requests > 0)
        {
            double avg_latency = static_cast<double>(total_latency_ms) / successful_requests;
            std::cout << "Average latency:     " << avg_latency << " ms\n";
            std::cout << "Min latency:         " << min_latency_ms << " ms\n";
            std::cout << "Max latency:         " << max_latency_ms << " ms\n";

            double success_rate = 100.0 * successful_requests / total_requests;
            std::cout << "Success rate:        " << success_rate << " %\n";
        }
        std::cout << "========================================\n";
    }
};

// Benchmark modes
void run_single_request(const std::string &host, int port, const std::string &prompt, int max_tokens)
{
    Logger::info("Running single request benchmark");

    BenchmarkClient client(host, port);

    if (!client.connect())
    {
        Logger::error("Failed to connect to orchestrator");
        return;
    }

    std::string response_text;
    double latency_ms;

    bool success = client.send_request(prompt, max_tokens, response_text, latency_ms);

    if (success)
    {
        std::cout << "\n========================================\n";
        std::cout << "Request successful!\n";
        std::cout << "========================================\n";
        std::cout << "Prompt:     " << prompt << "\n";
        std::cout << "Max tokens: " << max_tokens << "\n";
        std::cout << "Latency:    " << latency_ms << " ms\n";
        std::cout << "\nGenerated text:\n";
        std::cout << response_text << "\n";
        std::cout << "========================================\n";
    }
    else
    {
        Logger::error("Request failed");
    }
}

void run_throughput_benchmark(const std::string &host, int port, int num_requests,
                              const std::string &prompt, int max_tokens)
{
    Logger::info("Running throughput benchmark with " + std::to_string(num_requests) + " requests");

    BenchmarkStats stats;
    BenchmarkClient client(host, port);

    if (!client.connect())
    {
        Logger::error("Failed to connect to orchestrator");
        return;
    }

    auto start_time = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_requests; i++)
    {
        std::string response_text;
        double latency_ms;

        bool success = client.send_request(prompt, max_tokens, response_text, latency_ms);
        stats.add_result(success, latency_ms);

        if ((i + 1) % 10 == 0)
        {
            std::cout << "Progress: " << (i + 1) << "/" << num_requests << " requests\r" << std::flush;
        }
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    double total_time_sec = std::chrono::duration<double>(end_time - start_time).count();

    std::cout << "\n";
    stats.print_summary();
    std::cout << "Total time:          " << total_time_sec << " seconds\n";
    std::cout << "Throughput:          " << (stats.successful_requests / total_time_sec) << " req/sec\n";
    std::cout << "========================================\n";
}

void print_usage(const char *prog_name)
{
    std::cout << "Usage: " << prog_name << " [options]\n";
    std::cout << "\n";
    std::cout << "Options:\n";
    std::cout << "  -h, --host <host>       Orchestrator hostname (default: localhost)\n";
    std::cout << "  -p, --port <port>       Orchestrator port (default: 9000)\n";
    std::cout << "  -m, --mode <mode>       Benchmark mode: single, throughput (default: single)\n";
    std::cout << "  -n, --num <num>         Number of requests for throughput mode (default: 100)\n";
    std::cout << "  -t, --tokens <tokens>   Max tokens to generate (default: 50)\n";
    std::cout << "  --prompt <prompt>       Prompt text (default: 'Once upon a time')\n";
    std::cout << "  --help                  Show this help message\n";
    std::cout << "\n";
    std::cout << "Examples:\n";
    std::cout << "  # Single request\n";
    std::cout << "  " << prog_name << " --host clgpu014.clemson.cloudlab.us --prompt \"Hello world\"\n";
    std::cout << "\n";
    std::cout << "  # Throughput benchmark\n";
    std::cout << "  " << prog_name << " --host clgpu014.clemson.cloudlab.us --mode throughput --num 100\n";
    std::cout << "\n";
}

int main(int argc, char **argv)
{
    // Default parameters
    std::string host = "localhost";
    int port = 9000;
    std::string mode = "single";
    int num_requests = 100;
    int max_tokens = 50;
    std::string prompt = "Once upon a time";

    // Parse command line arguments
    for (int i = 1; i < argc; i++)
    {
        std::string arg = argv[i];

        if (arg == "--help")
        {
            print_usage(argv[0]);
            return 0;
        }
        else if ((arg == "-h" || arg == "--host") && i + 1 < argc)
        {
            host = argv[++i];
        }
        else if ((arg == "-p" || arg == "--port") && i + 1 < argc)
        {
            port = std::stoi(argv[++i]);
        }
        else if ((arg == "-m" || arg == "--mode") && i + 1 < argc)
        {
            mode = argv[++i];
        }
        else if ((arg == "-n" || arg == "--num") && i + 1 < argc)
        {
            num_requests = std::stoi(argv[++i]);
        }
        else if ((arg == "-t" || arg == "--tokens") && i + 1 < argc)
        {
            max_tokens = std::stoi(argv[++i]);
        }
        else if (arg == "--prompt" && i + 1 < argc)
        {
            prompt = argv[++i];
        }
        else
        {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    // Initialize logger
    Logger::init(Logger::LogLevel::INFO);

    std::cout << "========================================\n";
    std::cout << "Disaggregated LLM Benchmark Client\n";
    std::cout << "========================================\n";
    std::cout << "Orchestrator: " << host << ":" << port << "\n";
    std::cout << "Mode:         " << mode << "\n";
    std::cout << "Prompt:       " << prompt << "\n";
    std::cout << "Max tokens:   " << max_tokens << "\n";
    if (mode == "throughput")
    {
        std::cout << "Num requests: " << num_requests << "\n";
    }
    std::cout << "========================================\n\n";

    // Run benchmark
    if (mode == "single")
    {
        run_single_request(host, port, prompt, max_tokens);
    }
    else if (mode == "throughput")
    {
        run_throughput_benchmark(host, port, num_requests, prompt, max_tokens);
    }
    else
    {
        std::cerr << "Unknown mode: " << mode << "\n";
        print_usage(argv[0]);
        return 1;
    }

    return 0;
}
