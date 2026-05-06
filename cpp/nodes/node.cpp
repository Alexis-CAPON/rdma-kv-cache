#include "cpp/nodes/node.h"
#include "cpp/network/tcp_server.h"
#include "cpp/nodes/event_queue.h"
#include "cpp/nodes/worker_pool_node.h"
#include "cpp/bindings/node_accessor.h"
#include <csignal>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
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
      node_info_(),
      rdma_engine_(config_, node_info_),
      request_tracker_layer_(RequestTrackerLayer::MemoryLayout{
          .buffer_size_bytes = config.memory.kv_buffer_mb * 1024UL * 1024UL,
          .max_concurrent_requests = config.memory.max_concurrent_requests,
          .num_layers = static_cast<int>(config.num_layers),
          .layer_size_bytes = config.memory.layer_size_mb * 1024UL * 1024UL}),
      worker_pool_(config.worker_pool_size, event_queue_, epoll_worker_, config, config.node_id, node_info_, rdma_engine_, request_tracker_layer_, this)
{
    Logger::info("Initializing Node " + config.role + " : " + config.hostname + ":" +
                 std::to_string(config.server_socket_port) + " (server)");

    // Populating node_info_ with config
    node_info_.node_id = config_.node_id;
    node_info_.role = (config_.role == "prefill") ? NodeRole::PREFILL : NodeRole::DECODE;
}

bool Node::start()
{
    Logger::info("Starting Node...");

    if (config_.memory.kv_buffer_mb < config_.memory.required_mb)
    {
        Logger::error("kv_buffer_mb (" + std::to_string(config_.memory.kv_buffer_mb) +
                      ") < required (" + std::to_string(config_.memory.required_mb) + ")");
        return false;
    }

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

    if (config_.use_gpu)
    {
        Logger::info("Initializing RDMA GPUDirect...");
        if (!rdma_engine_.initialize())
        {
            Logger::error("Failed to initialize RDMA engine");
            epoll_worker_.stop();
            worker_pool_.stop();
            return false;
        }
        Logger::info("RDMA GPUDirect initialized successfully");
    }
    else
    {
        Logger::info("use_gpu=false — skipping RDMA GPUDirect initialization (MooncakeConnector path)");
    }

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
    // 6. Start RDMA polling thread (decode nodes only)
    // ========================================
    if (config_.role == "decode")
    {
        rdma_poll_running_ = true;
        rdma_poll_thread_ = std::thread(&Node::rdma_poll_loop, this);
        Logger::info("RDMA polling thread started for decode node");
    }

    // ========================================
    // 7. Set State
    // ========================================

    state_.store(State::WAITING_FOR_ORCHESTRATOR_BROADCAST);
    running_ = true;

    Logger::info("========================================");
    Logger::info("Node ONLINE (" + config_.role + ")");
    Logger::info("State: WAITING_FOR_ORCHESTRATOR_BROADCAST");
    Logger::info("Server port: " + std::to_string(config_.server_socket_port));
    Logger::info("Backend: " + std::string(config_.use_gpu ? "GPUDirect RDMA (RDMAConnector)" : "CPU RDMA (MooncakeConnector)"));
    if (config_.use_gpu)
    {
        Logger::info("GPU: " + node_info_.gpu_name + " (GPU" + std::to_string(node_info_.gpu_id) + ")");
        Logger::info("IB Device: " + node_info_.ib_dev_name + " port " + std::to_string(node_info_.ib_port));
    }
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

    // Stop RDMA polling thread (decode nodes only)
    if (config_.role == "decode" && rdma_poll_running_)
    {
        Logger::info("Stopping RDMA polling thread...");
        rdma_poll_running_ = false;
        if (rdma_poll_thread_.joinable())
        {
            rdma_poll_thread_.join();
        }
        Logger::info("RDMA polling thread stopped");
    }

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
    // Determine the KV connector role (prefill produces, decode consumes)
    std::string kv_role = (config_.role == "prefill") ? "kv_producer" : "kv_consumer";

    std::string cmd;

    // Construct venv activation and Python command
    std::string venv_path = std::string(getenv("HOME")) + "/rdma-kv-cache/venv";
    std::string activate_and_run = "source " + venv_path + "/bin/activate && ";

    if (config_.use_gpu)
    {
        // GPUDirect RDMA path: use our custom RDMAConnector
        // Build KV transfer config JSON with module path to our custom connector
        std::string project_root = std::string(getenv("HOME")) + "/rdma-kv-cache";
        std::string kv_transfer_config =
            "'{\"kv_connector\": \"RDMAConnector\", "
            "\"kv_role\": \"" +
            kv_role + "\", "
                      "\"kv_connector_module_path\": \"rdma_connector\"}'";

        // Add python directory and build directory to PYTHONPATH
        // - python/ contains rdma_connector.py
        // - build/cpp/bindings contains node_accessor.so and rdma_bindings.so
        cmd = activate_and_run +
              "PYTHONPATH=" + project_root + "/python:" +
              project_root + "/build/cpp/bindings:$PYTHONPATH "
                             "python -m vllm.entrypoints.openai.api_server "
                             "--model '" +
              config_.model_name + "' "
                                   "--port " +
              std::to_string(config_.vllm_port) + " "
                                                  "--gpu-memory-utilization " +
              std::to_string(config_.gpu_memory_utilization) + " "
                                                               "--kv-transfer-config " +
              kv_transfer_config;
    }
    else
    {
        // CPU / standard RDMA path: use MooncakeConnector
        // Generate the Mooncake JSON config from node and orchestrator addresses
        std::string mooncake_cfg_path = "/tmp/mooncake-" + config_.role + "-" +
                                        std::to_string(config_.client_socket_port) + ".json";

        // Use the first configured IB device, falling back to "mlx5_0"
        std::string rdma_device = config_.rdma.ib_devices.empty() ? "mlx5_0" : config_.rdma.ib_devices[0];

        std::string mooncake_cfg_content =
            "{\n"
            "  \"local_hostname\": \"" +
            config_.hostname + "\",\n"
                               "  \"metadata_server\": \"" +
            config_.orchestrator_host + ":2379\",\n"
                                        "  \"protocol\": \"rdma\",\n"
                                        "  \"rdma_devices\": [\"" +
            rdma_device + "\"],\n"
                          "  \"use_gpu_direct\": false\n"
                          "}\n";

        // Write mooncake config to a temp file before forking (mode 0600 for security)
        {
            int fd = open(mooncake_cfg_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
            if (fd < 0)
            {
                Logger::error("Failed to create Mooncake config at " + mooncake_cfg_path +
                              ": " + std::strerror(errno));
                return false;
            }
            ssize_t written = write(fd, mooncake_cfg_content.c_str(), mooncake_cfg_content.size());
            close(fd);
            if (written < 0 || static_cast<size_t>(written) != mooncake_cfg_content.size())
            {
                Logger::error("Failed to write Mooncake config to " + mooncake_cfg_path +
                              ": " + std::strerror(errno));
                return false;
            }
        }

        // Build KV transfer config JSON for Mooncake
        std::string kv_transfer_config =
            "'{\"kv_connector\": \"MooncakeConnector\", "
            "\"kv_role\": \"" +
            kv_role + "\"}'";

        cmd = activate_and_run + "MOONCAKE_CONFIG_PATH=" + mooncake_cfg_path + " "
                                                                               "python -m vllm.entrypoints.openai.api_server "
                                                                               "--model '" +
              config_.model_name + "' "
                                   "--port " +
              std::to_string(config_.vllm_port) + " "
                                                  "--kv-transfer-config " +
              kv_transfer_config + " "
                                   "--device cpu";
    }

    vllm_pid = fork();
    if (vllm_pid == 0)
    {
        // Child process - start VLLM server
        // Use bash explicitly for 'source' command support
        execl("/bin/bash", "bash", "-c", cmd.c_str(), (char *)NULL);
        // If execl returns, it means it failed
        Logger::error("Failed to start VLLM server with command: " + cmd);
        exit(1);
    }
    else if (vllm_pid < 0)
    {
        Logger::error("Failed to fork process for VLLM server");
        return false;
    }

    Logger::info("vLLM server started with PID " + std::to_string(vllm_pid) +
                 " (connector=" + std::string(config_.use_gpu ? "rdma_connector" : "MooncakeConnector") + ")");
    sleep(5); // Simple approach, or implement health check
    return true;
}

void Node::rdma_poll_loop()
{
    Logger::info("RDMA poll loop started");
    std::vector<ibv_wc> wcs;

    while (rdma_poll_running_)
    {
        // Exit immediately if the RDMA engine has entered a fatal state
        // (e.g. recv WR replenishment failed too many times — the recv queue
        // is about to drain and no further transfers can complete cleanly).
        if (rdma_engine_.is_rdma_fatal())
        {
            Logger::error("Node: RDMA engine fatal error detected — stopping poll loop. "
                          "Node must be restarted to recover.");
            rdma_poll_running_ = false;
            break;
        }

        int n = rdma_engine_.poll_recv_cq(wcs, 32);

        if (n <= 0)
        {
            // No completions, sleep briefly
            std::this_thread::sleep_for(std::chrono::microseconds(10));
            continue;
        }

        // Process completions
        for (int i = 0; i < n; ++i)
        {
            uint16_t seq_num, layer_id;
            RequestTrackerLayer::decode_layer_id(wcs[i].imm_data, seq_num, layer_id);

            std::string request_id = request_tracker_layer_.resolve_seq_num(seq_num);
            if (request_id.empty())
            {
                Logger::warning("RDMA completion: unknown seq_num " + std::to_string(seq_num));
                continue;
            }

            Logger::debug("RDMA layer received: request=" + request_id +
                          " seq_num=" + std::to_string(seq_num) +
                          " layer=" + std::to_string(layer_id));

            bool all_done = request_tracker_layer_.mark_layer_received(request_id, layer_id);

            if (all_done)
            {

#ifdef ENABLE_GPU_DIRECT
                // Fence: ensure all RDMA DMA writes to GPU HBM2 are visible
                // to CUDA kernels before we tell vLLM to start decode.
                cudaError_t fence_err = cudaDeviceSynchronize();
                if (fence_err != cudaSuccess)
                {
                    Logger::error("RDMA poll: cudaDeviceSynchronize failed: " +
                                  std::string(cudaGetErrorString(fence_err)));
                    // Do NOT push the event — decode would read garbage data.
                    continue;
                }
#endif

                // All layers received - push KV_TRANSFER_COMPLETE event to queue
                Logger::info("All layers received for request " + request_id + " — triggering decode");

                auto msg = std::make_unique<Message>();
                msg->type = MessageType::KV_TRANSFER_COMPLETE;
                msg->request_info.request_id = request_id;
                // Copy other needed fields from tracker if needed
                RequestTrackerLayer::RequestInfo info;
                if (request_tracker_layer_.get_request(request_id, info))
                {
                    msg->request_info.max_tokens = info.max_output_tokens;
                }

                event_queue_.push(Event(-1, std::move(msg)));
            }
        }
    }

    Logger::info("RDMA poll loop stopped");
}