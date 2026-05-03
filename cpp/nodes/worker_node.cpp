#include "cpp/nodes/worker_node.h"
#include "cpp/common/logger.h"
#include "cpp/nodes/node.h"
#include "cpp/network/http_client.h"
#include <chrono>
#include <algorithm>
#include <cstring>
#include <functional>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Constructor for prefill/decode nodes
WorkerNode::WorkerNode(
    uint32_t worker_id,
    EpollWorkerNode &epoll_worker,
    const Config &config,
    const std::string &node_id,
    std::atomic<uint64_t> *server_ops_counter,
    NodeInfo &node_info,
    RDMAEngine &rdma_engine,
    RequestTrackerLayer &request_tracker_layer,
    Node *node_ptr)
    : WorkerBase(worker_id, config, node_id, server_ops_counter),
      epoll_worker_(epoll_worker),
      node_ptr_(node_ptr),
      node_info_(node_info),
      rdma_engine_(rdma_engine),
      request_tracker_layer_(request_tracker_layer)
{
    Logger::info("WorkerNode " + std::to_string(worker_id_) + " initialized for Prefill/Decode node");
}

void WorkerNode::process_event(Event &event)
{
    events_processed_++;

    Logger::debug("WorkerNode " + std::to_string(worker_id_) +
                  " processing event from fd=" + std::to_string(event.client_fd));

    switch (event.message->type)
    {

    case MessageType::ASSIGN_REQUEST:
        handle_assign_request(event.client_fd, event.message.get());
        break;

        // Prefill/Decode nodes

    case MessageType::PREPARE_DECODE_SLOT:
        handle_prepare_decode_slot(event.client_fd, event.message.get());
        break;

    case MessageType::BROADCAST_MEMBER_INFO:
        handle_broadcast_member_info(event.message.get());
        break;

    case MessageType::TRANSFER_READY:
        handle_transfer_ready(event.client_fd, event.message.get());
        break;

    case MessageType::PREFILL_COMPLETE:
        handle_prefill_complete(event.client_fd, event.message.get());
        break;

    case MessageType::KV_TRANSFER_COMPLETE:
        handle_kv_transfer_complete(event.client_fd, event.message.get());
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

void WorkerNode::send_client_response(int client_fd, const Message &response)
{
    Logger::debug("WorkerNode " + std::to_string(worker_id_) +
                  " sending client response to fd=" + std::to_string(client_fd) +
                  " type=" + std::to_string(static_cast<int>(response.type)));

    epoll_worker_.enqueue_response(client_fd, response);
}

void WorkerNode::send_server_response(int server_fd, const Message &response)
{
    Logger::debug("Worker " + std::to_string(worker_id_) +
                  " sending server response to fd=" + std::to_string(server_fd) +
                  " type=" + std::to_string(static_cast<int>(response.type)));

    // Note: READ_RESPONSE and VOTE are sent back through ServerIOHandler
    epoll_worker_.enqueue_response(server_fd, response);
}
// ============================================================================
// Event Handlers for Prefill and Decode Nodes
// ============================================================================

// Used by prefill/decode node to handle the QP map and all the RDMA info by the orchestrator and connect all QP between all the node and then send confirmation to orchestator
void WorkerNode::handle_broadcast_member_info(Message *msg)
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
                // FIX BUG #5: Validate that peer provides QP info for us
                bool found = false;

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

                        Logger::info("Filled remote QP info for peer " + qmap.peer_node_id);
                        found = true;
                        break;
                    }
                }

                // FIX BUG #5: Validate QP info was found
                if (!found)
                {
                    Logger::error("CRITICAL: Peer " + peer.node_id +
                                  " did not provide QP info for this node (" + node_info_.node_id + ")");
                    Logger::error("Peer's qmaps contains " + std::to_string(peer.qmaps.size()) + " entries:");
                    for (const auto &pqmap : peer.qmaps)
                    {
                        Logger::error("  - peer_node_id=" + pqmap.peer_node_id +
                                      ", local_qp_num=" + std::to_string(pqmap.local_qp_num));
                    }
                    throw std::runtime_error("QP info exchange failed: peer " + peer.node_id +
                                             " missing QP mapping for " + node_info_.node_id);
                }

                // FIX BUG #5: Validate QP number is non-zero
                if (qmap.remote_qp_num == 0)
                {
                    Logger::error("CRITICAL: Invalid remote_qp_num=0 for peer " + peer.node_id);
                    throw std::runtime_error("Invalid QP number (0) received from peer " + peer.node_id);
                }

                Logger::debug("Validated remote QP info for peer " + peer.node_id +
                              ": remote_qp_num=" + std::to_string(qmap.remote_qp_num) +
                              ", remote_lid=" + std::to_string(qmap.remote_lid));
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
void WorkerNode::handle_transfer_ready(int server_fd, Message *msg)
{
    Logger::debug("Worker " + std::to_string(worker_id_) +
                  " handling TRANSFER_READY from fd=" + std::to_string(server_fd) +
                  " request_id=" + msg->request_info.request_id);
}

// Used by prefill node to handle assign request from orchestrator, which contains the request details and which decode node to send the request to
void WorkerNode::handle_assign_request(int orchestrator_fd, Message *msg)
{
    Logger::debug("Worker " + std::to_string(worker_id_) +
                  " handling ASSIGN_REQUEST from fd=" + std::to_string(orchestrator_fd) +
                  " request_id=" + msg->request_info.request_id);

    // Verify slot allocation was received correctly
    Logger::info("Prefill received: slot_id=" + std::to_string(msg->request_info.slot_id) +
                 ", slot_base_offset=" + std::to_string(msg->request_info.slot_base_offset));

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
        {"extra_body", {{"kv_connector_config", {{"role", "send"}, {"rdma_device", node_info_.ib_dev_name}, {"rdma_port", node_info_.ib_port}, {"rdma_gid_index", config_.rdma.gid_index}, {"peer_id", decode_node_id}, {"rdma_qp_num", decode_peer->local_qp_num}, {"rdma_remote_addr", decode_peer->remote_addr}, {"rdma_remote_rkey", decode_peer->remote_rkey}, {"layer_size", config_.memory.layer_size_mb * 1024 * 1024}, {"request_id", msg->request_info.request_id}, {"slot_id", msg->request_info.slot_id}, {"slot_base_offset", msg->request_info.slot_base_offset}, {"request_seq", msg->request_info.slot_id}}}}}};

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

void WorkerNode::handle_prefill_complete(int server_fd, Message *msg)
{
    const std::string &request_id = msg->request_info.request_id;
    int slot_id = msg->request_info.slot_id;
    int num_layers = msg->kv_metadata.num_layers;
    int num_tokens = msg->kv_metadata.num_tokens;
    std::string prefill_node_id = msg->request_info.prefill_node_id;

    // ← No vLLM call here. Return and wait for KV_TRANSFER_COMPLETE.
}

void WorkerNode::handle_kv_transfer_complete(int /*unused_fd*/, Message *msg)
{
    const std::string &request_id = msg->request_info.request_id;

    RequestTrackerLayer::RequestInfo info;
    if (!request_tracker_layer_.get_request(request_id, info))
    {
        Logger::error("KV_TRANSFER_COMPLETE for unknown request " + request_id);
        return;
    }

    request_tracker_layer_.update_state(
        request_id,
        RequestTrackerLayer::State::DECODING);

    // Now we know all layers are in the staging buffer at slot info.slot_id
    json vllm_request = {
        {"model", config_.model_name},
        {"prompt", info.prompt.empty() ? "" : info.prompt},
        {"max_tokens", info.max_output_tokens > 0 ? info.max_output_tokens : msg->request_info.max_tokens},
        {"temperature", 0.7},
        {"extra_body", {{"kv_connector_config", {{"role", "recv"}, {"rdma_device", node_info_.ib_dev_name}, {"rdma_port", node_info_.ib_port}, {"rdma_gid_index", config_.rdma.gid_index}, {"slot_id", info.slot_id}, {"slot_base_offset", info.base_offset}, {"layer_size", config_.memory.layer_size_mb * 1024 * 1024}, {"num_layers", info.num_layers}, {"num_tokens", info.num_tokens}, {"request_id", request_id}}}}}};

    std::string vllm_url = "http://localhost:" +
                           std::to_string(config_.vllm_port) +
                           "/v1/completions";

    HTTPClient http_client;
    auto response = http_client.post(vllm_url, vllm_request.dump());

    if (!response.success)
    {
        Logger::error("vLLM decode failed: " + response.error);
        request_tracker_layer_.update_state(request_id, RequestTrackerLayer::State::FAILED);
        return;
    }

    // Parse response
    json vllm_response = json::parse(response.body);
    std::string generated_text = vllm_response["choices"][0]["text"];

    Logger::info("Decode complete: " + generated_text);

    // Update state
    request_tracker_layer_.update_state(request_id, RequestTrackerLayer::State::COMPLETED);

    // Send DECODE_COMPLETE to orchestrator
    Message decode_complete_msg = Message::decode_complete(
        config_.orchestrator_id,
        msg->request_info,
        generated_text);
    send_server_response(epoll_worker_.get_orchestrator_fd(), decode_complete_msg);

    // Clean up
    request_tracker_layer_.remove_request(request_id);
}

void WorkerNode::handle_prepare_decode_slot(int server_fd, Message *msg)
{
    const std::string &request_id = msg->request_info.request_id;
    int slot_id = msg->request_info.slot_id;

    Logger::debug("Handling PREPARE_DECODE_SLOT for request " + request_id +
                  " slot_id=" + std::to_string(slot_id));

    request_tracker_layer_.add_request(
        msg->request_info.request_id,
        msg->request_info.prefill_node_id,
        msg->request_info.slot_id,
        config_.num_layers);

    request_tracker_layer_.update_state(request_id, RequestTrackerLayer::State::WAITING_FOR_KV);

    // Also store prompt + max_tokens into the tracker entry so handle_kv_transfer_complete has them
}