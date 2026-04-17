#include "cpp/nodes/worker.h"
#include "cpp/common/logger.h"
#include <chrono>
#include <algorithm>
#include <cstring>

Worker::Worker(
    uint32_t worker_id,
    EpollWorker &epoll_worker,
    const Config &config,
    uint64_t node_id,
    std::atomic<uint64_t> *server_ops_counter)
    : worker_id_(worker_id),
      node_id_(node_id),
      epoll_worker_(epoll_worker),
      config_(config),
      server_ops_counter_(server_ops_counter),
      events_processed_(0)
{
    Logger::info("Worker " + std::to_string(worker_id_) + " initialized");
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

    case MessageType::CLIENT_RESPONSE:
        handle_client_response(event.client_fd, event.message.get());
        break;

    case MessageType::CLIENT_ERROR:
        handle_client_error(event.client_fd, event.message.get());
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

    case MessageType::RDMA_REGISTRATION_COMPLETE:
        handle_node_rdma_registration_complete(event.client_fd, event.message.get());
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

    // Encode: timestamp (low 32 bits) | node_id (24 bits) | worker_id (8 bits)
    return (timestamp << 32) | ((node_id_ & 0xFFFFFF) << 8) | (worker_id_ & 0xFF);
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
};

// Used by orchestrator to respond to a client
void Worker::handle_client_response(int client_fd, Message *msg)
{
    Logger::debug("Worker " + std::to_string(worker_id_) +
                  " handling client response from fd=" + std::to_string(client_fd) +
                  " request_id=" + msg->request_info.request_id);
}

// Used by orchestrator to reponse an error to a client
void Worker::handle_client_error(int client_fd, Message *msg)
{
    Logger::debug("Worker " + std::to_string(worker_id_) +
                  " handling client error from fd=" + std::to_string(client_fd) +
                  " request_id=" + msg->request_info.request_id +
                  " error_message=" + msg->error_message);
};

// ============================================================================
// Server Message Handlers - Orchestrator Role
// ============================================================================
// Used by orchestrator to assign request to prefill node
void Worker::handle_assign_request(int server_fd, Message *msg)
{
    Logger::debug("Worker " + std::to_string(worker_id_) +
                  " handling ASSIGN_REQUEST from fd=" + std::to_string(server_fd) +
                  " request_id=" + msg->request_info.request_id);

    // Retrieve FD based on prefill_node_id in the message, and send the request assignment to the prefill node
}

// Used by orchestrator to process RDMA message received from prefill/decode node
void Worker::handle_process_rdma_registration(int server_fd, Message *msg)
{
    // We check for MAP we have, if it contain all the node required, if not we wait for other message, if we have all the node, we broadcast to all the node the member info, so they can start RDMA connection setup among themselves
    Logger::debug("Worker " + std::to_string(worker_id_) +
                  " handling RDMA_PROCESS_REGISTRATION from fd=" + std::to_string(server_fd) +
                  " node_id=" + std::to_string(msg->source_node_id));
}

// Used by orchestrator to handle RDMA_READY message from prefill/decode node, which indicate that the node is ready for RDMA communication (i.e. has completed RDMA connection setup with all other node and is ready for RDMA read/write)
void Worker::handle_ready(int server_fd, Message *msg)
{
    Logger::debug("Worker " + std::to_string(worker_id_) +
                  " handling RDMA_READY from fd=" + std::to_string(server_fd) +
                  " node_id=" + std::to_string(msg->source_node_id));
    // We wait for all the node to send RDMA_READY message, which indicate that all the node is ready for RDMA communication, then we can start processing client request and assign request to prefill node
}
// ============================================================================
// Server Message Handlers - Prefill/Decode Role
// ============================================================================
// Used by prefill/decode node to signal completion of RDMA registration to orchestrator
void Worker::handle_node_rdma_registration_complete(int server_fd, Message *msg)
{
    Logger::debug("Worker " + std::to_string(worker_id_) +
                  " handling RDMA_REGISTRATION_COMPLETE from fd=" + std::to_string(server_fd) +
                  " node_id=" + std::to_string(msg->source_node_id));

    // Need to create a RDMA PROCESS REGISTRATION MESSAGE TYPE to send to the orchestrator when we are done with registration, so the orchestrator can keep track of which node has completed RDMA registration and when all node have completed RDMA registration, the orchestrator can broadcast the member info to all the node so they can start connecting to each other with RDMA
}

// Used by prefill/decode node to signal request failure to orchestrator (e.g. due to GPU OOM)
void Worker::handle_request_failed(int server_fd, Message *msg)
{
    Logger::debug("Worker " + std::to_string(worker_id_) +
                  " handling REQUEST_FAILED from fd=" + std::to_string(server_fd) +
                  " request_id=" + msg->request_info.request_id +
                  " error_message=" + msg->error_message);
    // Inform orchestrator that request has failed, so orchestrator can cleanup and respond to client
}

// Used by prefill/decode node to handle the QP map and all the RDMA info by the orchestrator and connect all QP between all the node and then send confirmation to orchestator
void Worker::handle_broadcast_member_info(Message *msg)
{
    Logger::debug("Worker " + std::to_string(worker_id_) +
                  " handling BROADCAST_MEMBER_INFO for epoch=" +
                  std::to_string(msg->timestamp));

    // Parse the message, extract all the RDMA Info for all the node, connect to all the node with RDMA and then send confirmation to orchestator that we are ready for RDMA communication
}

// ===========================================================================
// Server Message Handlers - Prefill Role
// ===========================================================================
// Used by prefill node to inform orchestrator that prefill phase is complete
void Worker::handle_prefill_complete(int server_fd, Message *msg)
{
    Logger::debug("Worker " + std::to_string(worker_id_) +
                  " handling PREFILL_COMPLETE from fd=" + std::to_string(server_fd) +
                  " request_id=" + msg->request_info.request_id);
}

// Used by prefill node to signal decode node that request is ready for transfer (i.e. prefill is done and decode can start RDMA read)
void Worker::handle_transfer_ready(int server_fd, Message *msg)
{
    Logger::debug("Worker " + std::to_string(worker_id_) +
                  " handling TRANSFER_READY from fd=" + std::to_string(server_fd) +
                  " request_id=" + msg->request_info.request_id);
}

// ===========================================================================
// Server Message Handlers - Decode Role
// ===========================================================================
// Used by decode node to inform orchestrator that decode phase is complete
void Worker::handle_decode_complete(int server_fd, Message *msg)
{
    Logger::debug("Worker " + std::to_string(worker_id_) +
                  " handling DECODE_COMPLETE from fd=" + std::to_string(server_fd) +
                  " request_id=" + msg->request_info.request_id);
}
