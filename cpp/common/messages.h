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
    uint64_t transaction_id; // Unique ID for correlating requests/responses
    uint64_t timestamp;      // For latency measurements and ordering
    uint64_t source_node_id; // For routing and logging

    MessageType type;
    RequestStatus status;

    RequestInfo request_info;

    // Error message
    std::string error_message;

    // Constructors
    Message()
        : type(MessageType::CLIENT_ERROR),
          status(RequestStatus::ERROR)
    {
    }

    // Static factory methods for common messages
    static Message create_get(const std::string &key);
    static Message create_put(const std::string &key, const std::string &value);
    static Message create_delete(const std::string &key);

    static Message create_get_response(const std::string &value);
    static Message create_put_response(RequestStatus status);
    static Message create_multi_put_response(RequestStatus status);
    static Message create_delete_response(RequestStatus status);

    static Message create_error(const std::string &error_msg);

    static Message create_node_join(uint64_t node_id, const std::string &address, uint64_t epoch);
    static Message create_node_leave(uint64_t node_id, uint64_t epoch);
    static Message create_node_failed(uint64_t node_id, uint64_t epoch);
    static Message create_node_recovery(uint64_t node_id, uint64_t epoch);
    static Message create_membership_snapshot(
        const std::vector<std::pair<uint64_t, std::string>> &nodes, uint64_t epoch);
    static Message create_heartbeat(uint64_t node_id);
};

// Serialization functions
std::vector<uint8_t> serialize_message(const Message &message);
std::unique_ptr<Message> deserialize_message(const std::vector<uint8_t> &data);
