#include "cpp/apps/orchestrator/orchestrator_engine.h"
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

OrchestratorEngine::OrchestratorEngine(Config config)
    : config_(config),
      orchestrator_running_(false),
      state_(State::STARTING),
      client_tcp_server_(),
      server_tcp_server_(),
      event_queue_(config.orchestrator_event_queue_size),
      node_registry_(),
      request_router_(node_registry_, RequestRouter::RoutingPolicy::LEAST_LOADED),
      request_tracker_(),
      rdma_exchange_tracker_(config.expected_prefill_nodes, config.expected_decode_nodes),
      epoll_worker_(event_queue_, client_tcp_server_, server_tcp_server_, node_registry_),
      worker_pool_(config.worker_pool_size, event_queue_, epoll_worker_, config, config.node_id, node_registry_, rdma_exchange_tracker_, request_tracker_, request_router_)
{
    Logger::info("Initializing Orchestrator: " + config.hostname + ":" +
                 std::to_string(config.client_socket_port) + " (client), :" +
                 std::to_string(config.server_socket_port) + " (server)");

    Logger::info("Expected cluster: " + std::to_string(config.expected_prefill_nodes) +
                 " prefill nodes, " + std::to_string(config.expected_decode_nodes) +
                 " decode nodes");
}

bool OrchestratorEngine::start()
{
    Logger::info("Starting Orchestrator...");

    // ========================================
    // 1. Bind and Listen on TCP Servers
    // ========================================

    Logger::info("Starting TCP Server for client requests on port " +
                 std::to_string(config_.client_socket_port));

    if (!client_tcp_server_.bind(config_.client_socket_port))
    {
        Logger::error("Failed to bind client TCP server to port " +
                      std::to_string(config_.client_socket_port));
        return false;
    }

    if (!client_tcp_server_.listen())
    {
        Logger::error("Failed to listen on client TCP server");
        return false;
    }

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
    // 4. Set State
    // ========================================

    state_.store(State::WAITING_FOR_NODES);
    orchestrator_running_ = true;

    Logger::info("========================================");
    Logger::info("Orchestrator ONLINE");
    Logger::info("State: WAITING_FOR_NODES");
    Logger::info("Client port: " + std::to_string(config_.client_socket_port));
    Logger::info("Server port: " + std::to_string(config_.server_socket_port));
    Logger::info("========================================");

    return true;
}

void OrchestratorEngine::shutdown()
{
    if (!orchestrator_running_)
    {
        Logger::warning("Orchestrator already stopped");
        return;
    }

    Logger::info("Shutting down Orchestrator...");

    // Update state
    state_.store(State::STOPPED);

    // Stop accepting new connections
    Logger::info("Stopping TCP servers...");
    client_tcp_server_.close();
    server_tcp_server_.close();

    // Stop event loop (no more I/O)
    Logger::info("Stopping EpollWorker...");
    epoll_worker_.stop();

    // Stop worker pool (let workers finish current tasks)
    Logger::info("Stopping WorkerPool...");
    worker_pool_.stop();

    orchestrator_running_ = false;

    // Print final statistics
    auto stats = rdma_exchange_tracker_.get_stats();
    Logger::info("========================================");
    Logger::info("Orchestrator Shutdown Complete");
    Logger::info("Cluster stats:");
    Logger::info("  Registered prefill nodes: " + std::to_string(stats.registered_prefill_nodes) +
                 "/" + std::to_string(stats.expected_prefill_nodes));
    Logger::info("  Registered decode nodes: " + std::to_string(stats.registered_decode_nodes) +
                 "/" + std::to_string(stats.expected_decode_nodes));
    Logger::info("  Ready nodes: " + std::to_string(stats.ready_nodes));
    Logger::info("  Topology validated: " + std::string(stats.topology_validated ? "Yes" : "No"));
    Logger::info("  Cluster operational: " + std::string(stats.cluster_operational ? "Yes" : "No"));

    auto req_stats = request_tracker_.get_stats();
    Logger::info("Request stats:");
    Logger::info("  Total requests: " + std::to_string(req_stats.total_requests));
    Logger::info("  Completed: " + std::to_string(req_stats.completed_requests));
    Logger::info("  Failed: " + std::to_string(req_stats.failed_requests));
    if (req_stats.completed_requests > 0)
    {
        Logger::info("  Avg total latency: " + std::to_string(req_stats.avg_total_latency_ms) + " ms");
        Logger::info("  Avg prefill latency: " + std::to_string(req_stats.avg_prefill_latency_ms) + " ms");
        Logger::info("  Avg decode latency: " + std::to_string(req_stats.avg_decode_latency_ms) + " ms");
    }
    Logger::info("========================================");
}
