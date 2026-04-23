#include "cpp/nodes/node.h"
#include "cpp/network/tcp_server.h"
#include "cpp/nodes/event_queue.h"
#include "cpp/nodes/worker_pool.h"
#include <csignal>
#include <stdexcept>
#include <thread>
#include <chrono>
#include <iomanip>
#include "cpp/common/logger.h"

std::atomic<bool> running{true};

Node::Node(Config config)
    : config_(config),
      running_(false),
      state_(State::STARTING),
      server_tcp_server_(),
      event_queue_(config.orchestrator_event_queue_size),
      epoll_worker_(event_queue_, server_tcp_server_, node_info_),
      worker_pool_(config.worker_pool_size, event_queue_, epoll_worker_, config, config.node_id, node_info_, rdma_engine_),
      node_info_(),
      rdma_engine_(config_, node_info_)
{
    Logger::info("Initializing Node" + config.role + " : " + config.hostname + ":" +
                 std::to_string(config.server_socket_port) + " (server)");

    // Populating node_info_ with config
    node_info_.node_id = config_.node_id;
}

bool Node::start()
{
    Logger::info("Starting Node...");

    // ========================================
    // 1. Bind and Listen on TCP Servers
    // ========================================

    Logger::info("Starting TCP Server for node connections on port " +
                 std::to_string(config_.server_socket_port));

    if (!server_tcp_server_.bind(config_.server_socket_port))
    {
        Logger::error("Failed to bind server TCP server to port " +
                      std::to_string(config_.server_socket_port));
        return false;
    }

    if (!server_tcp_server_.listen())
    {
        Logger::error("Failed to listen on server TCP server");
        return false;
    }

    // ========================================
    // 2. Start EpollWorker (I/O Thread)
    // ========================================

    epoll_worker_.start();
    Logger::info("EpollWorker started");

    // ========================================
    // 3. Start WorkerPool (Processing Threads)
    // ========================================

    worker_pool_.start();
    Logger::info("WorkerPool started with " + std::to_string(config_.worker_pool_size) + " workers");

    // ========================================
    // 4. Initialize RDMA GPUDirect
    // ========================================

    Logger::info("Initializing RDMA GPUDirect...");
    if (!rdma_engine_.initialize())
    {
        Logger::error("Failed to initialize RDMA engine");
        epoll_worker_.stop();
        worker_pool_.stop();
        return false;
    }
    Logger::info("RDMA GPUDirect initialized successfully");

    // ==========================================
    // 5. Connect to Orchestrator Node and send RDMA Data
    // ==========================================

    Logger::info("Connecting to orchestrator...");
    if (!epoll_worker_.connect_to_orchestrator(config_.orchestrator_host, config_.orchestrator_port))
    {
        Logger::error("Failed to connect to orchestrator");
        rdma_engine_.shutdown();
        epoll_worker_.stop();
        worker_pool_.stop();
        return false;
    }

    if (!epoll_worker_.send_node_info_to_orchestrator())
    {
        Logger::error("Failed to send NodeInfo to orchestrator");
        rdma_engine_.shutdown();
        epoll_worker_.stop();
        worker_pool_.stop();
        return false;
    }

    // ========================================
    // 5. Set State
    // ========================================

    state_.store(State::WAITING_FOR_ORCHESTRATOR_BROADCAST);
    running_ = true;

    Logger::info("========================================");
    Logger::info("Node ONLINE (" + config_.role + ")");
    Logger::info("State: WAITING_FOR_ORCHESTRATOR_BROADCAST");
    Logger::info("Server port: " + std::to_string(config_.server_socket_port));
    Logger::info("GPU: " + node_info_.gpu_name + " (GPU" + std::to_string(node_info_.gpu_id) + ")");
    Logger::info("IB Device: " + node_info_.ib_dev_name + " port " + std::to_string(node_info_.ib_port));
    Logger::info("========================================");

    return true;
}

void Node::shutdown()
{
    if (!running_)
    {
        Logger::warning("Node already stopped");
        return;
    }

    Logger::info("Shutting down Node...");

    if (vllm_pid > 0)
    {
        Logger::info("Terminating VLLM server (PID " + std::to_string(vllm_pid) + ")...");
        kill(vllm_pid, SIGTERM);
        int status;
        waitpid(vllm_pid, &status, 0);
        Logger::info("VLLM server terminated");
    }

    // Update state
    state_.store(State::STOPPED);

    // Stop accepting new connections
    Logger::info("Stopping TCP servers...");
    server_tcp_server_.close();

    // Shutdown RDMA
    Logger::info("Shutting down RDMA...");
    rdma_engine_.shutdown();

    // Stop event loop (no more I/O)
    Logger::info("Stopping EpollWorker...");
    epoll_worker_.stop();

    // Stop worker pool (let workers finish current tasks)
    Logger::info("Stopping WorkerPool...");
    worker_pool_.stop();

    running_ = false;

    Logger::info("========================================");
    Logger::info("Node Shutdown Complete (" + config_.role + ")");
    Logger::info("========================================");
}

bool Node::start_vllm_server()
{
    set_current_node(this);

    vllm_pid = fork();
    if (vllm_pid == 0)
    {
        // Child process - start VLLM server
        std::string cmd = "python -m vllm.entrypoints.openai.api_server "
                          "--model " +
                          config_.model_name + " "
                                               "--kv-transfer-config " +
                          std::to_string(config_.size_local_buffer) + "," +
                          std::to_string(config_.memory.kv_buffer_mb) + "," +
                          std::to_string(config_.memory.chunk_size_mb) + " "
                                                                         "--port " +
                          std::to_string(config_.vllm_port) + " ";

        execl("/bin/sh", "sh", "-c", cmd.c_str(), (char *)NULL);
        // If execl returns, it means it failed
        Logger::error("Failed to start VLLM server with command: " + cmd);
        exit(1);
    }
    else if (vllm_pid < 0)
    {
        Logger::error("Failed to fork process for VLLM server");
        return false;
    }

    sleep(5); // Simple approach, or implement health check

    Logger::info("vLLM server started with PID " + std::to_string(vllm_pid));
    return true;
}