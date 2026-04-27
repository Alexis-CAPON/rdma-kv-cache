#include "cpp/nodes/worker.h"
#include "cpp/common/logger.h"
#include "cpp/apps/orchestrator/orchestrator_engine.h"
#include "cpp/nodes/node.h"
#include <chrono>
#include <algorithm>
#include <cstring>
#include <functional>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Constructor for prefill/decode nodes
Worker::Worker(
    uint32_t worker_id,
    EpollWorker &epoll_worker,
    const Config &config,
    const std::string &node_id,
    std::atomic<uint64_t> *server_ops_counter,
    NodeInfo &node_info,
    RDMAEngine &rdma_engine,
    Node *node_ptr)
    : worker_id_(worker_id),
      node_id_(node_id),
      epoll_worker_(epoll_worker),
      config_(config),
      server_ops_counter_(server_ops_counter),
      node_ptr_(node_ptr),
      node_info_(node_info),
      rdma_engine_(rdma_engine),
      events_processed_(0)
{
    Logger::info("Worker " + std::to_string(worker_id_) + " initialized for Prefill/Decode node");
}

// Constructor for orchestrator node
Worker::Worker(
    uint32_t worker_id,
    EpollWorker &epoll_worker,
    const Config &config,
    const std::string &node_id,
    std::atomic<uint64_t> *server_ops_counter,
    NodeRegistry &node_registry,
    RdmaExchangeTracker &rdma_exchange_tracker,
    RequestTrackerOrchestrator &request_tracker_orchestrator,
    RequestRouter &request_router)
    : worker_id_(worker_id),
      node_id_(node_id),
      epoll_worker_(epoll_worker),
      config_(config),
      server_ops_counter_(server_ops_counter),
      node_registry_(node_registry),
      rdma_exchange_tracker_(rdma_exchange_tracker),
      request_tracker_orchestrator_(request_tracker_orchestrator),
      request_router_(request_router),
      events_processed_(0)
{
    Logger::info("Worker " + std::to_string(worker_id_) + " initialized for Orchestrator node");
}

Worker::~Worker()
{
    Logger::info("Worker " + std::to_string(worker_id_) + " destroyed (" +
                 std::to_string(events_processed_) + " events processed)");
}

void Worker::process_event(Event &event)
{
    events_processed_++;

    Logger::debug("Worker " + std::to_string(worker_id_) +
                  " processing event from fd=" + std::to_string(event.client_fd));

    switch (event.message->type)
    {
    // Client requests
    case MessageType::CLIENT_REQUEST:
        handle_client_request(event.client_fd, event.message.get());
        break;
        // Orchestrator

    case MessageType::ASSIGN_REQUEST:
        handle_assign_request(event.client_fd, event.message.get());
        break;

    case MessageType::RDMA_PROCESS_REGISTRATION:
        handle_process_rdma_registration(event.client_fd, event.message.get());
        break;

    case MessageType::RDMA_READY:
        handle_ready(event.client_fd, event.message.get());
        break;

        // Prefill/Decode nodes

    case MessageType::BROADCAST_MEMBER_INFO:
        handle_broadcast_member_info(event.message.get());
        break;

    case MessageType::REQUEST_FAILED:
        handle_request_failed(event.client_fd, event.message.get());
        break;

    case MessageType::TRANSFER_READY:
        handle_transfer_ready(event.client_fd, event.message.get());
        break;

    case MessageType::PREFILL_COMPLETE:
        handle_prefill_complete(event.client_fd, event.message.get());
        break;

    case MessageType::PREFILL_COMPLETE_ORCHESTRATOR:
        handle_prefill_complete_orchestrator(event.client_fd, event.message.get());
        break;

    case MessageType::DECODE_COMPLETE:
        handle_decode_complete(event.client_fd, event.message.get());
        break;

    default:
        Logger::error("Worker " + std::to_string(worker_id_) +
                      " received unknown message type: " +
                      std::to_string(static_cast<int>(event.message->type)));
        break;
    }
}

// ============================================================================
// Response Handling
// ============================================================================

void Worker::send_client_response(int client_fd, const Message &response)
{
    Logger::debug("Worker " + std::to_string(worker_id_) +
                  " sending client response to fd=" + std::to_string(client_fd) +
                  " type=" + std::to_string(static_cast<int>(response.type)));

    epoll_worker_.enqueue_response(client_fd, response);
}

void Worker::send_server_response(int server_fd, const Message &response)
{
    Logger::debug("Worker " + std::to_string(worker_id_) +
                  " sending server response to fd=" + std::to_string(server_fd) +
                  " type=" + std::to_string(static_cast<int>(response.type)));

    // Note: READ_RESPONSE and VOTE are sent back through ServerIOHandler
    epoll_worker_.enqueue_response(server_fd, response);
}

// ============================================================================
// Helper Functions
// ============================================================================

uint64_t Worker::generate_transaction_id()
{
    // Generate globally unique transaction ID across all nodes and workers
    // Format: [32 bits: timestamp_low] [24 bits: node_id] [8 bits: worker_id]
    // Using timestamp ensures uniqueness even if counters reset
    auto now = std::chrono::steady_clock::now();
    uint64_t timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                             now.time_since_epoch())
                             .count();

    // Encode: timestamp (low 32 bits) | node_id hash (24 bits) | worker_id (8 bits)
    uint64_t node_hash = std::hash<std::string>{}(node_id_) & 0xFFFFFF;
    return (timestamp << 32) | (node_hash << 8) | (worker_id_ & 0xFF);
}

// ============================================================================
// Client Request Handlers - Orchestrator Role
// ============================================================================
// Used by orchestrator when processing a client request
void Worker::handle_client_request(int client_fd, Message *msg)
{
    Logger::debug("Worker " + std::to_string(worker_id_) +
                  " handling client request from fd=" + std::to_string(client_fd) +
                  " request_id=" + msg->request_info.request_id);

    // Generate request ID
    uint64_t transaction_id = generate_transaction_id();

    // Select a prefill and decode node to assign this request to (e.g., simple round-robin or more advanced load balancing)
    auto selected_prefill_node = request_router_.select_prefill_node();
    auto selected_decode_node = request_router_.select_decode_node_for_prefill(selected_prefill_node ? selected_prefill_node->node_id : "");

    if (!selected_prefill_node || !selected_decode_node)
    {
        Logger::error("No healthy nodes available to handle request_id=" + msg->request_info.request_id);
        Message error_response = Message::create_error("No healthy nodes available. Please try again later.");
        send_client_response(client_fd, error_response);
        return;
    }

    // Create a new RequestInfo object

    RequestInfo request_info;
    request_info.request_id = msg->request_info.request_id;
    request_info.prompt = msg->request_info.prompt;
    request_info.max_tokens = msg->request_info.max_tokens;
    request_info.prefill_node_id = selected_prefill_node->node_id;
    request_info.decode_node_id = selected_decode_node->node_id;
    request_info.timestamp_created = std::chrono::system_clock::now().time_since_epoch().count();

    // Create a new request entry in the RequestTracker using the RequestInfo and client_fd for response delivery later
    request_tracker_orchestrator_.track_request(request_info, client_fd);

    // Send an ASSIGN_REQUEST message to the selected prefill node with the request details and the selected decode node info
    Message assign_msg = Message::create_assign_request(config_.orchestrator_id, request_info);

    auto prefill_fd = node_registry_.get_node_fd(selected_prefill_node->node_id);
    if (!prefill_fd)
    {
        Logger::error("Failed to find prefill node fd for node_id=" + selected_prefill_node->node_id);
        return;
    }
    send_server_response(prefill_fd.value(), assign_msg);

    request_router_.increment_node_load(selected_prefill_node->node_id);
    request_router_.increment_node_load(selected_decode_node->node_id);

    Logger::info("Assigned request_id=" + request_info.request_id +
                 " to prefill_node_id=" + selected_prefill_node->node_id +
                 " and decode_node_id=" + selected_decode_node->node_id);

    // Note: The prefill node will then handle the ASSIGN_REQUEST, start the prefill phase, and upon completion, it will send a DECODE_COMPLETE message to the decode node to trigger the decode phase. The prefill node will also update the RequestTracker with the status changes
};

// ============================================================================
// Server Message Handlers - Orchestrator Role
// ============================================================================

void Worker::handle_prefill_complete_orchestrator(int server_fd, Message *msg)
{
    Logger::debug("Worker " + std::to_string(worker_id_) +
                  " handling PREFILL_COMPLETE_ORCHESTRATOR from fd=" + std::to_string(server_fd) +
                  " request_id=" + msg->request_info.request_id);

    // Update the RequestTracker with the prefill completion status for this request
    request_tracker_orchestrator_.update_status(msg->request_info.request_id, RequestStatus::DECODING);

    // Send a DECODE_COMPLETE message to the assigned decode node to trigger the decode phase for this request
    auto decode_node_fd = node_registry_.get_node_fd(msg->request_info.decode_node_id);
    if (decode_node_fd)
    {
        Message prefill_msg = Message::prefill_complete(config_.orchestrator_id, msg->request_info);
        send_server_response(decode_node_fd.value(), prefill_msg);
        Logger::info("Informed decode node_id=" + msg->request_info.decode_node_id +
                     " to start decoding for request_id=" + msg->request_info.request_id);
    }
    else
    {
        Logger::error("Failed to find decode node fd for node_id=" + msg->request_info.decode_node_id +
                      " when handling PREFILL_COMPLETE_ORCHESTRATOR for request_id=" + msg->request_info.request_id);
    }
}

// Used by orchestrator to process RDMA message received from prefill/decode node
void Worker::handle_process_rdma_registration(int server_fd, Message *msg)
{
    // We check for MAP we have, if it contain all the node required, if not we wait for other message, if we have all the node, we broadcast to all the node the member info, so they can start RDMA connection setup among themselves
    Logger::debug("Worker " + std::to_string(worker_id_) +
                  " handling RDMA_PROCESS_REGISTRATION from fd=" + std::to_string(server_fd) +
                  " node_id=" + msg->source_node_id);

    NodeInfo received_node_info = msg->node_info;

    // Update the node registry with the received RDMA registration info
    node_registry_.add_node(received_node_info);

    // Check if we have received RDMA registration from all expected nodes
    int total_registered_nodes = node_registry_.get_total_registered_nodes();

    int total_expected_nodes = config_.expected_prefill_nodes + config_.expected_decode_nodes;

    // If all nodes have registered, broadcast the member info to all the node so they can start connecting to each other with RDMA

    if (total_registered_nodes == total_expected_nodes)
    {
        Logger::info("All expected nodes have completed RDMA registration (" +
                     std::to_string(total_registered_nodes) + "/" +
                     std::to_string(total_expected_nodes) + "). Broadcasting member info...");

        // Retrieve all node info from the registry
        std::vector<NodeInfo> all_nodes = node_registry_.get_all_nodes();

        auto broadcast_msg = Message::create_broadcast_member_info(node_id_, all_nodes);

        // Broadcast to all registered nodes
        for (const auto &node : all_nodes)
        {
            auto node_fd = node_registry_.get_node_fd(node.node_id);
            if (!node_fd)
            {
                Logger::warning("No fd found for node_id=" + node.node_id + ", skipping broadcast");
                continue;
            }
            send_server_response(node_fd.value(), broadcast_msg);
            Logger::debug("Sent BROADCAST_MEMBER_INFO to node_id=" + node.node_id +
                          " at " + node.ip_address + ":" + std::to_string(node.tcp_port));
        }
    }
    // If not, just wait for more RDMA registration messages from other node
    else
    {
        Logger::info("Received RDMA registration from node_id=" + received_node_info.node_id +
                     " (" + std::to_string(total_registered_nodes) + "/" +
                     std::to_string(total_expected_nodes) + " registered)");
    }
}

// Used by orchestrator to handle RDMA_READY message from prefill/decode node, which indicate that the node is ready for RDMA communication (i.e. has completed RDMA connection setup with all other node and is ready for RDMA read/write)
void Worker::handle_ready(int server_fd, Message *msg)
{
    Logger::debug("Worker " + std::to_string(worker_id_) +
                  " handling RDMA_READY from fd=" + std::to_string(server_fd) +
                  " node_id=" + msg->source_node_id);
    // We wait for all the node to send RDMA_READY message, which indicate that all the node is ready for RDMA communication, then we can start processing client request and assign request to prefill node

    std::string node_id = msg->source_node_id;

    // Mark this node as ready in the RdmaExchangeTracker
    bool all_ready = rdma_exchange_tracker_.mark_node_ready(node_id);
    if (all_ready)
    {
        Logger::info("All nodes are ready for RDMA communication. Orchestrator can now start processing client requests and assigning them to prefill nodes.");
        OrchestratorEngine::set_state(OrchestratorEngine::State::RUNNING);
    }
}

// Used by decode node to inform orchestrator that decode phase is complete
void Worker::handle_decode_complete(int server_fd, Message *msg)
{
    Logger::debug("Worker " + std::to_string(worker_id_) +
                  " handling DECODE_COMPLETE from fd=" + std::to_string(server_fd) +
                  " request_id=" + msg->request_info.request_id);

    // Update the RequestTracker with the decode completion status for this request
    request_tracker_orchestrator_.update_status(msg->request_info.request_id, RequestStatus::COMPLETED);

    // Send the generated response back to the client
    auto client_fd = request_tracker_orchestrator_.get_client_fd(msg->request_info.request_id);
    if (client_fd)
    {
        Message client_response = Message::create_client_response(config_.orchestrator_id, msg->request_info, msg->response_text);
        send_client_response(client_fd.value(), client_response);
        Logger::info("Sent response to client for request_id=" + msg->request_info.request_id);
    }
    else
    {
        Logger::error("Failed to find client fd for request_id=" + msg->request_info.request_id + " when handling DECODE_COMPLETE");
    }
}

// Used by prefill/decode node to signal request failure to orchestrator (e.g. due to GPU OOM)
void Worker::handle_request_failed(int server_fd, Message *msg)
{
    Logger::debug("Worker " + std::to_string(worker_id_) +
                  " handling REQUEST_FAILED from fd=" + std::to_string(server_fd) +
                  " request_id=" + msg->request_info.request_id +
                  " error_message=" + msg->error_message);
    // Update the RequestTracker with the failure status for this request
    request_tracker_orchestrator_.update_status(msg->request_info.request_id, RequestStatus::FAILED);

    // Send an error response back to the client
    auto client_fd = request_tracker_orchestrator_.get_client_fd(msg->request_info.request_id);
    if (client_fd)
    {
        Message error_response = Message::create_request_failed(config_.orchestrator_id, msg->request_info, msg->error_message);
        send_client_response(client_fd.value(), error_response);
        Logger::info("Sent error response to client for request_id=" + msg->request_info.request_id + " error_message=" + msg->error_message);
    }
    else
    {
        Logger::error("Failed to find client fd for request_id=" + msg->request_info.request_id + " when handling REQUEST_FAILED");
    }
}
// ============================================================================
// Server Message Handlers - Prefill/Decode Role
// ============================================================================

// Used by prefill/decode node to handle the QP map and all the RDMA info by the orchestrator and connect all QP between all the node and then send confirmation to orchestator
void Worker::handle_broadcast_member_info(Message *msg)
{
    Logger::debug("Worker " + std::to_string(worker_id_) +
                  " handling BROADCAST_MEMBER_INFO for epoch=" +
                  std::to_string(msg->timestamp));

    // Parse the message, extract all the RDMA Info for all the node, connect to all the node with RDMA and then send confirmation to orchestator that we are ready for RDMA communication

    std::vector<NodeInfo> membership_list_info = msg->membership_list_info;

    // We update our node_info_.qmaps based on the membership_list_info we receive from the orchestrator, and then we can start connecting to other node with RDMA based on the updated node_info_

    for (const auto &peer : membership_list_info)
    {
        // Skip ourselves

        if (peer.node_id == node_info_.node_id)
        {
            continue;
        }

        /*

        struct PeerInfoandQPs
        {
            std::string peer_node_id; // Who this QP is for

            // Local QP info (this node's side of connection)
            uint32_t local_qp_num;
            uint16_t local_lid;
            uint8_t local_gid[16];
            uint32_t local_psn;

            // Remote QP info (peer node's side of connection)
            uint32_t remote_qp_num;
            uint16_t remote_lid;
            uint8_t remote_gid[16];
            uint32_t remote_psn;
            uint64_t remote_rkey;
            uint64_t remote_addr;
        };

        // Node information
        struct NodeInfo
        {
            std::string node_id;
            NodeRole role;
            std::string ip_address;
            uint16_t tcp_port;         // For orchestrator communication
            int node_listen_socket_fd; // TCP connection fd to this node
            uint16_t vllm_port;        // vLLM HTTP port
            bool is_healthy;

            // ── GPUDirect RDMA Device Info ───────────────────────────────────────────────
            int gpu_id;           // CUDA device ID (0-based)
            std::string gpu_name; // GPU model name (e.g., "Tesla V100")
            int gpu_numa;         // NUMA node of GPU (-1 if unavailable)
            size_t gpu_mem_bytes; // Total GPU memory

            std::string ib_dev_name; // InfiniBand device (e.g., "mlx5_0")
            int ib_port;             // IB port number (usually 1)
            int ib_numa;             // NUMA node of HCA (-1 if unavailable)
            uint64_t ib_speed_gbps;  // Link speed in Gb/s

            // ── Memory Region Info ───────────────────────────────────────────────────────
            uint64_t gpu_base_addr;  // GPU memory region base address (d_ptr)
            uint64_t lkey;           // Local key for this node's MR
            uint64_t rkey;           // Remote key for this node's MR (shared with peers)
            size_t memory_pool_size; // Size of GPU memory region in bytes

            // ── Queue Pair Info ──────────────────────────────────────────────────────────
            std::vector<PeerInfoandQPs> qmaps; // QP connection info for each peer

            // Example for prefill-01 connecting to 3 decode nodes:
            // qmaps: [
            //   {peer_id: "decode-01", local_qp_num: 100, ..., remote_qp_num: 200, ...},
            //   {peer_id: "decode-02", local_qp_num: 101, ..., remote_qp_num: 201, ...},
            //   {peer_id: "decode-03", local_qp_num: 102, ..., remote_qp_num: 202, ...}
            // ]
        };


        */

        // NEED TO VERIFY THIS

        for (auto &qmap : node_info_.qmaps)
        {
            if (qmap.peer_node_id == peer.node_id)
            {
                // Find the QP info that peer created for US
                for (const auto &peer_qmap : peer.qmaps)
                {
                    if (peer_qmap.peer_node_id == node_info_.node_id)
                    {
                        // This is the peer's QP that connects to us
                        qmap.remote_qp_num = peer_qmap.local_qp_num;
                        qmap.remote_lid = peer_qmap.local_lid;
                        memcpy(qmap.remote_gid, peer_qmap.local_gid, 16);
                        qmap.remote_psn = peer_qmap.local_psn;
                        qmap.remote_rkey = peer.rkey;
                        qmap.remote_addr = peer.gpu_base_addr;

                        Logger::info("Filled remote QP info for peer " +
                                     qmap.peer_node_id);
                        break;
                    }
                }
            }
        }
    }

    // Connect to all QPs (GPUDirect RDMA path only)

    if (config_.use_gpu)
    {
        if (!rdma_engine_.connect_all_qps())
        {
            Logger::error("Failed to connect QP with peers");
        }
        Logger::info("Successfully connected QP with peers");
    }
    else
    {
        Logger::info("use_gpu=false — skipping RDMA QP connection (MooncakeConnector path)");
    }

    // Start VLLM Server

    node_ptr_->start_vllm_server();

    // Send RDMA_READY message to orchestrator to indicate that we are ready for RDMA communication after we finish connecting to all the node with RDMA
    auto ready_msg = Message::create_rdma_ready(node_info_.node_id);

    send_server_response(epoll_worker_.get_orchestrator_fd(), ready_msg);

    node_ptr_->set_state(Node::State::RUNNING);

    Logger::debug("Sent RDMA_READY to orchestrator for node_id=" + node_info_.node_id);
}

// ===========================================================================
// Server Message Handlers - Prefill Role
// ===========================================================================
// Used by prefill node to signal decode node that request is ready for transfer (i.e. prefill is done and decode can start RDMA read)
void Worker::handle_transfer_ready(int server_fd, Message *msg)
{
    Logger::debug("Worker " + std::to_string(worker_id_) +
                  " handling TRANSFER_READY from fd=" + std::to_string(server_fd) +
                  " request_id=" + msg->request_info.request_id);
}

// Used by prefill node to handle assign request from orchestrator, which contains the request details and which decode node to send the request to
void Worker::handle_assign_request(int orchestrator_fd, Message *msg)
{
    Logger::debug("Worker " + std::to_string(worker_id_) +
                  " handling ASSIGN_REQUEST from fd=" + std::to_string(orchestrator_fd) +
                  " request_id=" + msg->request_info.request_id);

    // 1. Get decode node RDMA info
    std::string decode_node_id = msg->request_info.decode_node_id;

    PeerInfoandQPs *decode_peer = nullptr;
    for (auto &qmap : node_info_.qmaps)
    {
        if (qmap.peer_node_id == decode_node_id)
        {
            decode_peer = &qmap;
            break;
        }
    }

    if (!decode_peer)
    {
        Logger::error("No RDMA info for decode node " + decode_node_id);
        return;
    }

    // 2. Build vLLM request with RDMA config
    json vllm_request = {
        {"model", config_.model_name},
        {"prompt", msg->request_info.prompt},
        {"max_tokens", 1}, // Prefill only
        {"temperature", 0.0},
        {"extra_body", {{"kv_connector_config", {{"role", "send"}, {"rdma_device", node_info_.ib_dev_name}, {"rdma_port", node_info_.ib_port}, {"rdma_gid_index", config_.rdma.gid_index}, {"peer_id", decode_node_id}, {"rdma_qp_num", decode_peer->local_qp_num}, {"rdma_remote_addr", decode_peer->remote_addr}, {"rdma_remote_rkey", decode_peer->remote_rkey}, {"layer_size", config_.memory.kv_buffer_mb * 1024 * 1024}, {"request_id", msg->request_info.request_id}}}}}};

    // 3. HTTP POST to local vLLM
    std::string vllm_url = "http://localhost:" +
                           std::to_string(config_.vllm_port) +
                           "/v1/completions";

    HTTPClient http_client;
    auto response = http_client.post(vllm_url, vllm_request.dump());

    if (!response.success)
    {
        Logger::error("vLLM prefill failed: " + response.error);
        // TODO: Send REQUEST_FAILED to orchestrator
        return;
    }

    // 4. Parse response
    json vllm_response = json::parse(response.body);
    int num_tokens = vllm_response["usage"]["prompt_tokens"];

    Logger::info("Prefill complete: " + std::to_string(num_tokens) + " tokens");

    // 5. Send PREFILL_COMPLETE to orchestrator
    Message prefill_complete_msg;
    prefill_complete_msg.type = MessageType::PREFILL_COMPLETE_ORCHESTRATOR;
    prefill_complete_msg.request_info = msg->request_info;
    prefill_complete_msg.request_info.timestamp_prefill_done =
        std::chrono::system_clock::now().time_since_epoch().count();

    // Add KV metadata
    prefill_complete_msg.kv_metadata.num_tokens = num_tokens;
    prefill_complete_msg.kv_metadata.num_layers = config_.num_layers;

    send_server_response(epoll_worker_.get_orchestrator_fd(), prefill_complete_msg);

    // Send PREFILL_COMPLETE message to orchestrator with request_id and prefill details (e.g. where the prefill data is stored in GPU memory, how big it is, etc.)
}

// ===========================================================================
// Server Message Handlers - Decode Role
// ===========================================================================

void Worker::handle_prefill_complete(int server_fd, Message *msg)
{
    // 1. Extract KV metadata
    std::string request_id = msg->request_info.request_id;
    int num_prompt_tokens = msg->kv_metadata.num_tokens;
    int num_layers = msg->kv_metadata.num_layers;

    // 2. Build vLLM request
    json vllm_request = {
        {"model", config_.model_name},
        {"prompt", ""}, // Empty - using cached KV
        {"max_tokens", msg->request_info.max_tokens},
        {"temperature", 0.7},
        {"extra_body", {{"kv_connector_config", {{"role", "recv"}, {"rdma_device", node_info_.ib_dev_name}, {"rdma_port", node_info_.ib_port}, {"rdma_gid_index", config_.rdma.gid_index}, {"layer_size", config_.memory.kv_buffer_mb * 1024 * 1024}, {"num_layers", num_layers}, {"num_tokens", num_prompt_tokens}, {"request_id", request_id}}}}}};

    // 3. HTTP POST to local vLLM
    std::string vllm_url = "http://localhost:" +
                           std::to_string(config_.vllm_port) +
                           "/v1/completions";

    HTTPClient http_client;
    auto response = http_client.post(vllm_url, vllm_request.dump());

    if (!response.success)
    {
        Logger::error("vLLM decode failed: " + response.error);
        return;
    }

    // 4. Parse response
    json vllm_response = json::parse(response.body);
    std::string generated_text = vllm_response["choices"][0]["text"];

    Logger::info("Decode complete: " + generated_text);

    // 5. Send DECODE_COMPLETE to orchestrator
    Message decode_complete_msg = Message::decode_complete(config_.orchestrator_id, msg->request_info, generated_text);

    send_server_response(epoll_worker_.get_orchestrator_fd(), decode_complete_msg);
}
