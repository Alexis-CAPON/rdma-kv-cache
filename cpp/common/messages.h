#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <memory>
#include <cstring>
#include <unordered_map>
#include "cpp/common/types.h"

struct Message
{
    uint64_t transaction_id;    // Unique ID for correlating requests/responses
    uint64_t timestamp;         // For latency measurements and ordering
    std::string source_node_id; // For routing and logging

    MessageType type;
    RequestStatus status;

    RequestInfo request_info;

    NodeInfo node_info;                         // Used for membership messages
    std::vector<NodeInfo> membership_list_info; // For broadcasting cluster state

    // Error message
    std::string error_message;

    // KV Meta data

    KVMetaData kv_metadata;

    std::string response_text; // For client responses

    // Constructors
    Message()
    {
    }

    // Static factory methods for common messages
    static Message create_rdma_process_registration(const std::string source_node_id, NodeInfo node_info);

    static Message create_broadcast_member_info(const std::string source_node_id, std::vector<NodeInfo> membership_list_info);
    static Message create_rdma_ready(const std::string source_node_id);

    static Message create_prepare_decode_slot(const std::string source_node_id, const RequestInfo &request_info);

    static Message create_assign_request(const std::string source_node_id, const RequestInfo &request_info);
    static Message prefill_complete(const std::string source_node_id, const RequestInfo &request_info);
    static Message decode_complete(const std::string source_node_id, const RequestInfo &request_info, const std::string &response_text);
    static Message create_request_failed(const std::string source_node_id, const RequestInfo &request_info, const std::string &error_message);
    static Message create_prefill_complete_orchestrator(const std::string source_node_id, const RequestInfo &request_info);
    static Message create_client_response(const std::string source_node_id, const RequestInfo &request_info, const std::string &response_text);

    static Message create_error(const std::string &error_msg);
};

// Serialization functions (free functions so callers don't need a Message instance)
std::vector<uint8_t> serialize_message(const Message &message);
std::unique_ptr<Message> deserialize_message(const std::vector<uint8_t> &data);