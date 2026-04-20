#include "cpp/common/messages.h"
#include "cpp/common/logger.h"
#include <cstring>
#include <cstdint>
#include <arpa/inet.h> // for htonl/ntohl
#include <memory>
#include <vector>
#include <string>

// Write a uint8_t
void write_uint8(std::vector<uint8_t> &buffer, uint8_t value)
{
    buffer.push_back(value);
}

// Write a uint32_t (network byte order)
void write_uint32(std::vector<uint8_t> &buffer, uint32_t value)
{
    uint32_t network_value = htonl(value);
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&network_value);
    buffer.insert(buffer.end(), bytes, bytes + 4);
}

// Write a uint64_t (network byte order)
void write_uint64(std::vector<uint8_t> &buffer, uint64_t value)
{
    // Split into two 32-bit values for network byte order
    uint32_t high = htonl(static_cast<uint32_t>(value >> 32));
    uint32_t low = htonl(static_cast<uint32_t>(value & 0xFFFFFFFF));

    const uint8_t *high_bytes = reinterpret_cast<const uint8_t *>(&high);
    const uint8_t *low_bytes = reinterpret_cast<const uint8_t *>(&low);

    buffer.insert(buffer.end(), high_bytes, high_bytes + 4);
    buffer.insert(buffer.end(), low_bytes, low_bytes + 4);
}

// Write a string (length-prefixed)
void write_string(std::vector<uint8_t> &buffer, const std::string &str)
{
    uint32_t length = static_cast<uint32_t>(str.size());
    write_uint32(buffer, length);
    buffer.insert(buffer.end(), str.begin(), str.end());
}

// Write a vector of strings (count + each string)
void write_string_vector(std::vector<uint8_t> &buffer, const std::vector<std::string> &vec)
{
    uint32_t count = static_cast<uint32_t>(vec.size());
    write_uint32(buffer, count);
    for (const auto &str : vec)
    {
        write_string(buffer, str);
    }
}

// Read a uint8_t
bool read_uint8(const uint8_t *&ptr, const uint8_t *end, uint8_t &value)
{
    if (ptr + 1 > end)
    {
        return false;
    }
    value = *ptr;
    ptr += 1;
    return true;
}

// Read a uint32_t (network byte order)
bool read_uint32(const uint8_t *&ptr, const uint8_t *end, uint32_t &value)
{
    if (ptr + 4 > end)
    {
        return false;
    }
    uint32_t network_value;
    std::memcpy(&network_value, ptr, 4);
    value = ntohl(network_value);
    ptr += 4;
    return true;
}

// Read a uint64_t (network byte order)
bool read_uint64(const uint8_t *&ptr, const uint8_t *end, uint64_t &value)
{
    if (ptr + 8 > end)
    {
        return false;
    }

    uint32_t high, low;
    std::memcpy(&high, ptr, 4);
    std::memcpy(&low, ptr + 4, 4);

    high = ntohl(high);
    low = ntohl(low);

    value = (static_cast<uint64_t>(high) << 32) | low;
    ptr += 8;
    return true;
}

// Read a string (length-prefixed)
bool read_string(const uint8_t *&ptr, const uint8_t *end, std::string &str)
{
    uint32_t length;
    if (!read_uint32(ptr, end, length))
    {
        return false;
    }

    if (ptr + length > end)
    {
        return false;
    }

    str.assign(reinterpret_cast<const char *>(ptr), length);
    ptr += length;
    return true;
}

// Read a vector of strings (count + each string)
bool read_string_vector(const uint8_t *&ptr, const uint8_t *end, std::vector<std::string> &vec)
{
    uint32_t count;
    if (!read_uint32(ptr, end, count))
    {
        return false;
    }

    vec.clear();
    vec.reserve(count);
    for (uint32_t i = 0; i < count; i++)
    {
        std::string str;
        if (!read_string(ptr, end, str))
        {
            return false;
        }
        vec.push_back(std::move(str));
    }
    return true;
}

// Factory methods
Message Message::create_rdma_process_registration(std::string source_node_id, NodeInfo node_info)
{
    Message msg;
    msg.type = MessageType::RDMA_PROCESS_REGISTRATION;
    msg.source_node_id = source_node_id;
    msg.timestamp = std::chrono::system_clock::now().time_since_epoch().count();
    msg.node_info = node_info;
    return msg;
}

Message Message::create_broadcast_member_info(std::string source_node_id, std::vector<NodeInfo> membership_list_info)
{
    Message msg;
    msg.type = MessageType::BROADCAST_MEMBER_INFO;
    msg.source_node_id = source_node_id;
    msg.timestamp = std::chrono::system_clock::now().time_since_epoch().count();
    msg.membership_list_info = membership_list_info;
    return msg;
}

Message Message::create_rdma_ready(std::string source_node_id)
{
    Message msg;
    msg.type = MessageType::RDMA_READY;
    msg.source_node_id = source_node_id;
    msg.timestamp = std::chrono::system_clock::now().time_since_epoch().count();
    return msg;
}

Message Message::create_assign_request(std::string source_node_id, const RequestInfo &request_info)
{
    Message msg;
    msg.type = MessageType::ASSIGN_REQUEST;
    msg.source_node_id = source_node_id;
    msg.timestamp = std::chrono::system_clock::now().time_since_epoch().count();
    msg.request_info = request_info;
    return msg;
}

Message Message::create_error(const std::string &error_msg)
{
    Message msg;
    msg.type = MessageType::CLIENT_ERROR;
    msg.error_message = error_msg;
    msg.timestamp = std::chrono::system_clock::now().time_since_epoch().count();
    return msg;
}

// Serialize message to bytes
std::vector<uint8_t> serialize_message(const Message &message)
{
    std::vector<uint8_t> buffer;
    buffer.reserve(256); // Pre-allocate reasonable size

    // Write message type (1 byte)
    write_uint8(buffer, static_cast<uint8_t>(message.type));

    // Write status code (1 byte)
    write_uint8(buffer, static_cast<uint8_t>(message.status));

    // Write transaction_id (8 bytes)
    write_uint64(buffer, message.transaction_id);

    // Write version (8 bytes)
    write_uint64(buffer, message.version);

    // Write key (length-prefixed string)
    write_string(buffer, message.key);

    // Write value (length-prefixed string)
    write_string(buffer, message.value);

    // Write keys vector (for MULTI_PUT)
    write_string_vector(buffer, message.keys);

    // Write values vector (for MULTI_PUT)
    write_string_vector(buffer, message.values);

    // Write error_message (length-prefixed string)
    write_string(buffer, message.error_message);

    // Write request_id (8 bytes)
    write_uint64(buffer, message.request_id);

    // Write membership_epoch (8 bytes)
    write_uint64(buffer, message.membership_epoch);

    // Write node_id (8 bytes)
    write_uint64(buffer, message.node_id);

    // Write node_address (length-prefixed string)
    write_string(buffer, message.node_address);

    // Write membership_list (length-prefixed string)
    write_string(buffer, message.membership_list);

    // Write RDMA info (struct fields)
    write_uint32(buffer, message.rdma_info.qp_num);                                 // 4 bytes
    buffer.push_back(static_cast<uint8_t>(message.rdma_info.lid >> 8));             // 2 bytes (high byte)
    buffer.push_back(static_cast<uint8_t>(message.rdma_info.lid & 0xFF));           // (low byte)
    buffer.insert(buffer.end(), message.rdma_info.gid, message.rdma_info.gid + 16); // 16 bytes
    write_uint64(buffer, message.rdma_info.memory_base_addr);                       // 8 bytes
    write_uint32(buffer, message.rdma_info.rkey);                                   // 4 bytes
    write_uint64(buffer, message.rdma_info.memory_size);                            // 8 bytes

    // Write QP map (count + entries)
    write_uint32(buffer, static_cast<uint32_t>(message.qp_map.size()));
    for (const auto &[node_id, qp_num] : message.qp_map)
    {
        write_uint64(buffer, node_id); // 8 bytes
        write_uint32(buffer, qp_num);  // 4 bytes
    }

    return buffer;
}

// Deserialize bytes to message
std::unique_ptr<Message> deserialize_message(const std::vector<uint8_t> &data)
{
    if (data.empty())
    {
        Logger::error("Cannot deserialize empty message");
        return nullptr;
    }

    const uint8_t *ptr = data.data();
    const uint8_t *end = data.data() + data.size();

    auto message = std::make_unique<Message>();

    // Read message type (1 byte)
    uint8_t type_byte;
    if (!read_uint8(ptr, end, type_byte))
    {
        Logger::error("Failed to read message type");
        return nullptr;
    }
    message->type = static_cast<MessageType>(type_byte);

    // Read status code (1 byte)
    uint8_t status_byte;
    if (!read_uint8(ptr, end, status_byte))
    {
        Logger::error("Failed to read status code");
        return nullptr;
    }
    message->status = static_cast<StatusCode>(status_byte);

    // Read transaction_id (8 bytes)
    if (!read_uint64(ptr, end, message->transaction_id))
    {
        Logger::error("Failed to read transaction_id");
        return nullptr;
    }

    // Read version (8 bytes)
    if (!read_uint64(ptr, end, message->version))
    {
        Logger::error("Failed to read version");
        return nullptr;
    }

    // Read key
    if (!read_string(ptr, end, message->key))
    {
        Logger::error("Failed to read key");
        return nullptr;
    }

    // Read value
    if (!read_string(ptr, end, message->value))
    {
        Logger::error("Failed to read value");
        return nullptr;
    }

    // Read keys vector
    if (!read_string_vector(ptr, end, message->keys))
    {
        Logger::error("Failed to read keys vector");
        return nullptr;
    }

    // Read values vector
    if (!read_string_vector(ptr, end, message->values))
    {
        Logger::error("Failed to read values vector");
        return nullptr;
    }

    // Read error_message
    if (!read_string(ptr, end, message->error_message))
    {
        Logger::error("Failed to read error_message");
        return nullptr;
    }

    // Read request_id (8 bytes)
    if (!read_uint64(ptr, end, message->request_id))
    {
        Logger::error("Failed to read request_id");
        return nullptr;
    }

    // Read membership_epoch (8 bytes)
    if (!read_uint64(ptr, end, message->membership_epoch))
    {
        Logger::error("Failed to read membership_epoch");
        return nullptr;
    }

    // Read node_id (8 bytes)
    if (!read_uint64(ptr, end, message->node_id))
    {
        Logger::error("Failed to read node_id");
        return nullptr;
    }

    // Read node_address
    if (!read_string(ptr, end, message->node_address))
    {
        Logger::error("Failed to read node_address");
        return nullptr;
    }

    // Read membership_list
    if (!read_string(ptr, end, message->membership_list))
    {
        Logger::error("Failed to read membership_list");
        return nullptr;
    }

    // Read RDMA info (struct fields)
    if (!read_uint32(ptr, end, message->rdma_info.qp_num))
    {
        Logger::error("Failed to read rdma_info.qp_num");
        return nullptr;
    }

    // Read uint16_t lid manually (2 bytes)
    if (ptr + 2 > end)
    {
        Logger::error("Not enough bytes for rdma_info.lid");
        return nullptr;
    }
    message->rdma_info.lid = (static_cast<uint16_t>(ptr[0]) << 8) | static_cast<uint16_t>(ptr[1]);
    ptr += 2;

    // Read rdma_gid (16 bytes)
    if (end - ptr < 16)
    {
        Logger::error("Not enough bytes for rdma_info.gid");
        return nullptr;
    }
    memcpy(message->rdma_info.gid, ptr, 16);
    ptr += 16;

    if (!read_uint64(ptr, end, message->rdma_info.memory_base_addr))
    {
        Logger::error("Failed to read rdma_info.memory_base_addr");
        return nullptr;
    }

    if (!read_uint32(ptr, end, message->rdma_info.rkey))
    {
        Logger::error("Failed to read rdma_info.rkey");
        return nullptr;
    }

    if (!read_uint64(ptr, end, message->rdma_info.memory_size))
    {
        Logger::error("Failed to read rdma_info.memory_size");
        return nullptr;
    }

    // Read QP map (count + entries)
    uint32_t qp_map_size;
    if (!read_uint32(ptr, end, qp_map_size))
    {
        Logger::error("Failed to read qp_map size");
        return nullptr;
    }

    message->qp_map.clear();
    for (uint32_t i = 0; i < qp_map_size; i++)
    {
        uint64_t node_id;
        uint32_t qp_num;

        if (!read_uint64(ptr, end, node_id))
        {
            Logger::error("Failed to read qp_map node_id");
            return nullptr;
        }

        if (!read_uint32(ptr, end, qp_num))
        {
            Logger::error("Failed to read qp_map qp_num");
            return nullptr;
        }

        message->qp_map[node_id] = qp_num;
    }

    // Verify we consumed all bytes
    if (ptr != end)
    {
        Logger::warning("Deserialized message but " +
                        std::to_string(end - ptr) + " bytes remain");
    }

    return message;
}