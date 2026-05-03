#include "cpp/nodes/worker_orchestrator.h"
#include "cpp/common/logger.h"
#include "cpp/apps/orchestrator/orchestrator_engine.h"
#include "cpp/nodes/node.h"
#include <chrono>
#include <algorithm>
#include <cstring>
#include <functional>
#include "cpp/nodes/epoll_worker_orchestrator.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

// Constructor for orchestrator node
WorkerOrchestrator::WorkerOrchestrator(
    uint32_t worker_id,
    EpollWorkerOrchestrator &epoll_worker,
    const Config &config,
    const std::string &node_id,
    std::atomic<uint64_t> *server_ops_counter,
    NodeRegistry &node_registry,
    RdmaExchangeTracker &rdma_exchange_tracker,
    RequestTrackerOrchestrator &request_tracker_orchestrator,
    RequestRouter &request_router)
    : WorkerBase(worker_id, config, node_id, server_ops_counter),
      epoll_worker_(epoll_worker),
      node_registry_(node_registry),
      rdma_exchange_tracker_(rdma_exchange_tracker),
      request_tracker_orchestrator_(request_tracker_orchestrator),
      request_router_(request_router)
{
    Logger::info("Worker " + std::to_string(worker_id_) + " initialized for Orchestrator node");
}

void WorkerOrchestrator::process_event(Event &event)
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

    case MessageType::REQUEST_FAILED:
        handle_request_failed(event.client_fd, event.message.get());
        break;

    case MessageType::PREFILL_COMPLETE_ORCHESTRATOR:
        handle_prefill_complete_orchestrator(event.client_fd, event.message.get());
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

void WorkerOrchestrator::send_client_response(int client_fd, const Message &response)
{
    Logger::debug("Worker " + std::to_string(worker_id_) +
                  " sending client response to fd=" + std::to_string(client_fd) +
                  " type=" + std::to_string(static_cast<int>(response.type)));

    epoll_worker_.enqueue_response(client_fd, response);
}

void WorkerOrchestrator::send_server_response(int server_fd, const Message &response)
{
    Logger::debug("Worker " + std::to_string(worker_id_) +
                  " sending server response to fd=" + std::to_string(server_fd) +
                  " type=" + std::to_string(static_cast<int>(response.type)));

    // Note: READ_RESPONSE and VOTE are sent back through ServerIOHandler
    epoll_worker_.enqueue_response(server_fd, response);
}

// ============================================================================
// Client Request Handlers - Orchestrator Role
// ============================================================================
// Used by orchestrator when processing a client request
void WorkerOrchestrator::handle_client_request(int client_fd, Message *msg)
{
    Logger::debug("Worker " + std::to_string(worker_id_) +
                  " handling client request from fd=" + std::to_string(client_fd) +
                  " request_id=" + msg->request_info.request_id);

    // Generate request ID
    uint64_t transaction_id = generate_transaction_id();

    // NEW: Use router's integrated slot allocation
    // This atomically selects prefill node, decode node, and allocates memory slot
    auto routing = request_router_.select_nodes_and_allocate_slot(msg->request_info.request_id);

    if (!routing)
    {
        Logger::error("No healthy nodes with available slots for request_id=" + msg->request_info.request_id);
        Message error_response = Message::create_error(
            "All nodes at capacity. Please try again later.");
        send_client_response(client_fd, error_response);
        return;
    }

    // Create RequestInfo with slot information
    RequestInfo request_info;
    request_info.request_id = msg->request_info.request_id;
    request_info.prompt = msg->request_info.prompt;
    request_info.max_tokens = msg->request_info.max_tokens;
    request_info.prefill_node_id = routing->prefill_node.node_id;
    request_info.decode_node_id = routing->decode_node.node_id;

    // NEW: Include slot allocation info
    request_info.slot_id = routing->slot_id;
    request_info.slot_base_offset = routing->slot_base_offset;

    request_info.timestamp_created = std::chrono::system_clock::now().time_since_epoch().count();

    // Create a new request entry in the RequestTracker using the RequestInfo and client_fd for response delivery later
    request_tracker_orchestrator_.track_request(request_info, client_fd);

    // Send an ASSIGN_REQUEST message to the selected prefill node with the request details and the selected decode node info
    Message assign_msg = Message::create_assign_request(config_.orchestrator_id, request_info);

    Message decode_prep_msg = Message::create_prepare_decode_slot(config_.orchestrator_id, request_info);

    auto decode_node_fd = node_registry_.get_node_fd(routing->decode_node.node_id);
    if (!decode_node_fd)
    {
        Logger::error("Failed to find decode node fd for node_id=" + routing->decode_node.node_id);
        // Free slot on error
        request_router_.free_slot(request_info.request_id);
        return;
    }
    send_server_response(decode_node_fd.value(), decode_prep_msg);

    auto prefill_fd = node_registry_.get_node_fd(routing->prefill_node.node_id);
    if (!prefill_fd)
    {
        Logger::error("Failed to find prefill node fd for node_id=" + routing->prefill_node.node_id);
        // Free slot on error
        request_router_.free_slot(request_info.request_id);
        return;
    }
    send_server_response(prefill_fd.value(), assign_msg);

    request_router_.increment_node_load(routing->prefill_node.node_id);
    request_router_.increment_node_load(routing->decode_node.node_id);

    Logger::info("Assigned request_id=" + request_info.request_id +
                 " to prefill=" + routing->prefill_node.node_id +
                 ", decode=" + routing->decode_node.node_id +
                 ", slot=" + std::to_string(routing->slot_id) +
                 ", offset=0x" + std::to_string(routing->slot_base_offset));

    // Note: The prefill node will then handle the ASSIGN_REQUEST, start the prefill phase, and upon completion, it will send a DECODE_COMPLETE message to the decode node to trigger the decode phase. The prefill node will also update the RequestTracker with the status changes
};

// ============================================================================
// Server Message Handlers - Orchestrator Role
// ============================================================================

void WorkerOrchestrator::handle_prefill_complete_orchestrator(int server_fd, Message *msg)
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
        // FIX: Properly handle failure - undo load increments and free slot
        Logger::error("Failed to find decode node fd for node_id=" + msg->request_info.decode_node_id +
                      " when handling PREFILL_COMPLETE_ORCHESTRATOR for request_id=" + msg->request_info.request_id);

        // Update status to FAILED
        request_tracker_orchestrator_.update_status(msg->request_info.request_id, RequestStatus::FAILED);

        // Free slot allocation
        request_router_.free_slot(msg->request_info.request_id);

        // Decrement load tracking for both nodes (undo the increments from handle_client_request)
        request_router_.decrement_node_load(msg->request_info.prefill_node_id);
        request_router_.decrement_node_load(msg->request_info.decode_node_id);

        // Send error response to client
        auto client_fd = request_tracker_orchestrator_.get_client_fd(msg->request_info.request_id);
        if (client_fd)
        {
            std::string error_msg = "Decode node " + msg->request_info.decode_node_id + " not found";
            Message error_response = Message::create_request_failed(config_.orchestrator_id, msg->request_info, error_msg);
            send_client_response(client_fd.value(), error_response);
            Logger::info("Sent error response to client for request_id=" + msg->request_info.request_id);
        }
    }
}

// Used by orchestrator to process RDMA message received from prefill/decode node
void WorkerOrchestrator::handle_process_rdma_registration(int server_fd, Message *msg)
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
void WorkerOrchestrator::handle_ready(int server_fd, Message *msg)
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
void WorkerOrchestrator::handle_decode_complete(int server_fd, Message *msg)
{
    Logger::debug("Worker " + std::to_string(worker_id_) +
                  " handling DECODE_COMPLETE from fd=" + std::to_string(server_fd) +
                  " request_id=" + msg->request_info.request_id);

    // Update the RequestTracker with the decode completion status for this request
    request_tracker_orchestrator_.update_status(msg->request_info.request_id, RequestStatus::COMPLETED);

    // NEW: Free slot allocation for this request
    request_router_.free_slot(msg->request_info.request_id);

    // Decrement load tracking
    request_router_.decrement_node_load(msg->request_info.prefill_node_id);
    request_router_.decrement_node_load(msg->request_info.decode_node_id);

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
void WorkerOrchestrator::handle_request_failed(int server_fd, Message *msg)
{
    Logger::debug("Worker " + std::to_string(worker_id_) +
                  " handling REQUEST_FAILED from fd=" + std::to_string(server_fd) +
                  " request_id=" + msg->request_info.request_id +
                  " error_message=" + msg->error_message);
    // Update the RequestTracker with the failure status for this request
    request_tracker_orchestrator_.update_status(msg->request_info.request_id, RequestStatus::FAILED);

    // NEW: Free slot allocation for failed request
    request_router_.free_slot(msg->request_info.request_id);

    // Decrement load tracking
    request_router_.decrement_node_load(msg->request_info.prefill_node_id);
    request_router_.decrement_node_load(msg->request_info.decode_node_id);

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