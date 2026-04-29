#include "cpp/common/messages.h"
#include "cpp/common/logger.h"
#include <arpa/inet.h> // htonl / ntohl
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// ── Low-level write helpers ───────────────────────────────────────────────────

static void write_uint8(std::vector<uint8_t> &buf, uint8_t v)
{
    buf.push_back(v);
}

static void write_uint16(std::vector<uint8_t> &buf, uint16_t v)
{
    buf.push_back(static_cast<uint8_t>(v >> 8));
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
}

static void write_uint32(std::vector<uint8_t> &buf, uint32_t v)
{
    uint32_t nv = htonl(v);
    const uint8_t *p = reinterpret_cast<const uint8_t *>(&nv);
    buf.insert(buf.end(), p, p + 4);
}

static void write_uint64(std::vector<uint8_t> &buf, uint64_t v)
{
    uint32_t hi = htonl(static_cast<uint32_t>(v >> 32));
    uint32_t lo = htonl(static_cast<uint32_t>(v & 0xFFFFFFFF));
    const uint8_t *ph = reinterpret_cast<const uint8_t *>(&hi);
    const uint8_t *pl = reinterpret_cast<const uint8_t *>(&lo);
    buf.insert(buf.end(), ph, ph + 4);
    buf.insert(buf.end(), pl, pl + 4);
}

static void write_string(std::vector<uint8_t> &buf, const std::string &s)
{
    write_uint32(buf, static_cast<uint32_t>(s.size()));
    buf.insert(buf.end(), s.begin(), s.end());
}

// ── Low-level read helpers ────────────────────────────────────────────────────

static bool read_uint8(const uint8_t *&p, const uint8_t *end, uint8_t &v)
{
    if (p + 1 > end)
        return false;
    v = *p++;
    return true;
}

static bool read_uint16(const uint8_t *&p, const uint8_t *end, uint16_t &v)
{
    if (p + 2 > end)
        return false;
    v = (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]);
    p += 2;
    return true;
}

static bool read_uint32(const uint8_t *&p, const uint8_t *end, uint32_t &v)
{
    if (p + 4 > end)
        return false;
    uint32_t nv;
    std::memcpy(&nv, p, 4);
    v = ntohl(nv);
    p += 4;
    return true;
}

static bool read_uint64(const uint8_t *&p, const uint8_t *end, uint64_t &v)
{
    if (p + 8 > end)
        return false;
    uint32_t hi, lo;
    std::memcpy(&hi, p, 4);
    std::memcpy(&lo, p + 4, 4);
    v = (static_cast<uint64_t>(ntohl(hi)) << 32) | ntohl(lo);
    p += 8;
    return true;
}

static bool read_string(const uint8_t *&p, const uint8_t *end, std::string &s)
{
    uint32_t len;
    if (!read_uint32(p, end, len))
        return false;
    if (static_cast<ptrdiff_t>(len) > end - p)
        return false;
    s.assign(reinterpret_cast<const char *>(p), len);
    p += len;
    return true;
}

// ── PeerInfoandQPs helpers ────────────────────────────────────────────────────

static void write_peer_info(std::vector<uint8_t> &buf, const PeerInfoandQPs &q)
{
    write_string(buf, q.peer_node_id);
    write_uint32(buf, q.local_qp_num);
    write_uint16(buf, q.local_lid);
    buf.insert(buf.end(), q.local_gid, q.local_gid + 16);
    write_uint32(buf, q.local_psn);
    write_uint32(buf, q.remote_qp_num);
    write_uint16(buf, q.remote_lid);
    buf.insert(buf.end(), q.remote_gid, q.remote_gid + 16);
    write_uint32(buf, q.remote_psn);
    write_uint64(buf, q.remote_rkey);
    write_uint64(buf, q.remote_addr);
}

static bool read_peer_info(const uint8_t *&p, const uint8_t *end, PeerInfoandQPs &q)
{
    if (!read_string(p, end, q.peer_node_id))
        return false;
    if (!read_uint32(p, end, q.local_qp_num))
        return false;
    if (!read_uint16(p, end, q.local_lid))
        return false;
    if (end - p < 16)
        return false;
    std::memcpy(q.local_gid, p, 16);
    p += 16;
    if (!read_uint32(p, end, q.local_psn))
        return false;
    if (!read_uint32(p, end, q.remote_qp_num))
        return false;
    if (!read_uint16(p, end, q.remote_lid))
        return false;
    if (end - p < 16)
        return false;
    std::memcpy(q.remote_gid, p, 16);
    p += 16;
    if (!read_uint32(p, end, q.remote_psn))
        return false;
    if (!read_uint64(p, end, q.remote_rkey))
        return false;
    if (!read_uint64(p, end, q.remote_addr))
        return false;
    return true;
}

// ── NodeInfo helpers ──────────────────────────────────────────────────────────
// File-descriptor fields (node_own_orchestrator_fd, node_other_node_fd) are
// process-local state and are NOT transmitted over the wire.

static void write_node_info(std::vector<uint8_t> &buf, const NodeInfo &n)
{
    write_string(buf, n.node_id);
    write_uint8(buf, static_cast<uint8_t>(n.role));
    write_string(buf, n.ip_address);
    write_uint16(buf, n.tcp_port);
    write_uint16(buf, n.vllm_port);
    write_uint8(buf, n.is_healthy ? 1 : 0);
    write_uint32(buf, static_cast<uint32_t>(n.gpu_id));
    write_string(buf, n.gpu_name);
    write_uint32(buf, static_cast<uint32_t>(n.gpu_numa));
    write_uint64(buf, static_cast<uint64_t>(n.gpu_mem_bytes));
    write_string(buf, n.ib_dev_name);
    write_uint32(buf, static_cast<uint32_t>(n.ib_port));
    write_uint32(buf, static_cast<uint32_t>(n.ib_numa));
    write_uint64(buf, n.ib_speed_gbps);
    write_uint64(buf, n.gpu_base_addr);
    write_uint64(buf, n.lkey);
    write_uint64(buf, n.rkey);
    write_uint64(buf, static_cast<uint64_t>(n.memory_pool_size));
    write_uint32(buf, static_cast<uint32_t>(n.qmaps.size()));
    for (const auto &q : n.qmaps)
        write_peer_info(buf, q);
}

static bool read_node_info(const uint8_t *&p, const uint8_t *end, NodeInfo &n)
{
    if (!read_string(p, end, n.node_id))
        return false;
    uint8_t role_byte;
    if (!read_uint8(p, end, role_byte))
        return false;
    n.role = static_cast<NodeRole>(role_byte);
    if (!read_string(p, end, n.ip_address))
        return false;
    if (!read_uint16(p, end, n.tcp_port))
        return false;
    if (!read_uint16(p, end, n.vllm_port))
        return false;
    uint8_t healthy_byte;
    if (!read_uint8(p, end, healthy_byte))
        return false;
    n.is_healthy = (healthy_byte != 0);
    uint32_t gpu_id_u;
    if (!read_uint32(p, end, gpu_id_u))
        return false;
    n.gpu_id = static_cast<int>(gpu_id_u);
    if (!read_string(p, end, n.gpu_name))
        return false;
    uint32_t gpu_numa_u;
    if (!read_uint32(p, end, gpu_numa_u))
        return false;
    n.gpu_numa = static_cast<int>(gpu_numa_u);
    uint64_t gpu_mem;
    if (!read_uint64(p, end, gpu_mem))
        return false;
    n.gpu_mem_bytes = static_cast<size_t>(gpu_mem);
    if (!read_string(p, end, n.ib_dev_name))
        return false;
    uint32_t ib_port_u;
    if (!read_uint32(p, end, ib_port_u))
        return false;
    n.ib_port = static_cast<int>(ib_port_u);
    uint32_t ib_numa_u;
    if (!read_uint32(p, end, ib_numa_u))
        return false;
    n.ib_numa = static_cast<int>(ib_numa_u);
    if (!read_uint64(p, end, n.ib_speed_gbps))
        return false;
    if (!read_uint64(p, end, n.gpu_base_addr))
        return false;
    if (!read_uint64(p, end, n.lkey))
        return false;
    if (!read_uint64(p, end, n.rkey))
        return false;
    uint64_t mem_pool;
    if (!read_uint64(p, end, mem_pool))
        return false;
    n.memory_pool_size = static_cast<size_t>(mem_pool);
    uint32_t qmap_count;
    if (!read_uint32(p, end, qmap_count))
        return false;
    n.qmaps.resize(qmap_count);
    for (auto &q : n.qmaps)
        if (!read_peer_info(p, end, q))
            return false;
    n.node_own_orchestrator_fd = -1; // process-local, not transmitted
    return true;
}

// ── RequestInfo helpers ───────────────────────────────────────────────────────

static void write_request_info(std::vector<uint8_t> &buf, const RequestInfo &r)
{
    write_string(buf, r.request_id);
    write_string(buf, r.prompt);
    write_uint32(buf, static_cast<uint32_t>(r.max_tokens));
    write_string(buf, r.prefill_node_id);
    write_string(buf, r.decode_node_id);

    // Serialize slot allocation
    write_uint32(buf, static_cast<uint32_t>(r.slot_id));
    write_uint64(buf, r.slot_base_offset);

    write_uint64(buf, r.timestamp_created);
    write_uint64(buf, r.timestamp_prefill_start);
    write_uint64(buf, r.timestamp_prefill_done);
    write_uint64(buf, r.timestamp_decode_start);
    write_uint64(buf, r.timestamp_decode_done);
}

static bool read_request_info(const uint8_t *&p, const uint8_t *end, RequestInfo &r)
{
    if (!read_string(p, end, r.request_id))
        return false;
    if (!read_string(p, end, r.prompt))
        return false;
    uint32_t max_tokens_u;
    if (!read_uint32(p, end, max_tokens_u))
        return false;
    r.max_tokens = static_cast<int>(max_tokens_u);
    if (!read_string(p, end, r.prefill_node_id))
        return false;
    if (!read_string(p, end, r.decode_node_id))
        return false;

    // Deserialize slot allocation
    uint32_t slot_id_u;
    if (!read_uint32(p, end, slot_id_u))
        return false;
    r.slot_id = static_cast<int>(slot_id_u);
    if (!read_uint64(p, end, r.slot_base_offset))
        return false;

    if (!read_uint64(p, end, r.timestamp_created))
        return false;
    if (!read_uint64(p, end, r.timestamp_prefill_start))
        return false;
    if (!read_uint64(p, end, r.timestamp_prefill_done))
        return false;
    if (!read_uint64(p, end, r.timestamp_decode_start))
        return false;
    if (!read_uint64(p, end, r.timestamp_decode_done))
        return false;
    return true;
}

// ── Timestamp helper ──────────────────────────────────────────────────────────

static uint64_t now_ns()
{
    return static_cast<uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count());
}

// ── Factory methods ───────────────────────────────────────────────────────────

Message Message::create_rdma_process_registration(const std::string source_node_id, NodeInfo node_info)
{
    Message msg;
    msg.type = MessageType::RDMA_PROCESS_REGISTRATION;
    msg.source_node_id = source_node_id;
    msg.timestamp = now_ns();
    msg.node_info = std::move(node_info);
    return msg;
}

Message Message::create_broadcast_member_info(const std::string source_node_id, std::vector<NodeInfo> membership_list_info)
{
    Message msg;
    msg.type = MessageType::BROADCAST_MEMBER_INFO;
    msg.source_node_id = source_node_id;
    msg.timestamp = now_ns();
    msg.membership_list_info = std::move(membership_list_info);
    return msg;
}

Message Message::create_rdma_ready(const std::string source_node_id)
{
    Message msg;
    msg.type = MessageType::RDMA_READY;
    msg.source_node_id = source_node_id;
    msg.timestamp = now_ns();
    return msg;
}

Message Message::create_assign_request(const std::string source_node_id, const RequestInfo &request_info)
{
    Message msg;
    msg.type = MessageType::ASSIGN_REQUEST;
    msg.source_node_id = source_node_id;
    msg.timestamp = now_ns();
    msg.request_info = request_info;
    return msg;
}

Message Message::prefill_complete(const std::string source_node_id, const RequestInfo &request_info)
{
    Message msg;
    msg.type = MessageType::PREFILL_COMPLETE;
    msg.source_node_id = source_node_id;
    msg.timestamp = now_ns();
    msg.request_info = request_info;
    return msg;
}

Message Message::decode_complete(const std::string source_node_id, const RequestInfo &request_info, const std::string &response_text)
{
    Message msg;
    msg.type = MessageType::DECODE_COMPLETE;
    msg.source_node_id = source_node_id;
    msg.timestamp = now_ns();
    msg.request_info = request_info;
    msg.response_text = response_text;
    return msg;
}

Message Message::create_request_failed(const std::string source_node_id, const RequestInfo &request_info, const std::string &error_message)
{
    Message msg;
    msg.type = MessageType::REQUEST_FAILED;
    msg.source_node_id = source_node_id;
    msg.timestamp = now_ns();
    msg.request_info = request_info;
    msg.error_message = error_message;
    return msg;
}

Message Message::create_prefill_complete_orchestrator(const std::string source_node_id, const RequestInfo &request_info)
{
    Message msg;
    msg.type = MessageType::PREFILL_COMPLETE_ORCHESTRATOR;
    msg.source_node_id = source_node_id;
    msg.timestamp = now_ns();
    msg.request_info = request_info;
    return msg;
}

Message Message::create_client_response(const std::string source_node_id, const RequestInfo &request_info, const std::string &response_text)
{
    Message msg;
    msg.type = MessageType::CLIENT_RESPONSE;
    msg.source_node_id = source_node_id;
    msg.timestamp = now_ns();
    msg.request_info = request_info;
    msg.response_text = response_text;
    return msg;
}

Message Message::create_error(const std::string &error_msg)
{
    Message msg;
    msg.type = MessageType::CLIENT_ERROR;
    msg.error_message = error_msg;
    msg.timestamp = now_ns();
    return msg;
}

// ── Serialization (free functions) ───────────────────────────────────────────
// Wire format (in order):
//   type (1B) | status (1B) | transaction_id (8B) | timestamp (8B)
//   | source_node_id (str) | request_info | node_info
//   | membership_list_info count (4B) + N × node_info
//   | error_message (str) | kv_metadata.num_layers (4B)
//   | kv_metadata.num_tokens (4B) | response_text (str)

std::vector<uint8_t> serialize_message(const Message &message)
{
    std::vector<uint8_t> buf;
    buf.reserve(512);

    write_uint8(buf, static_cast<uint8_t>(message.type));
    write_uint8(buf, static_cast<uint8_t>(message.status));
    write_uint64(buf, message.transaction_id);
    write_uint64(buf, message.timestamp);
    write_string(buf, message.source_node_id);

    write_request_info(buf, message.request_info);
    write_node_info(buf, message.node_info);

    write_uint32(buf, static_cast<uint32_t>(message.membership_list_info.size()));
    for (const auto &ni : message.membership_list_info)
        write_node_info(buf, ni);

    write_string(buf, message.error_message);
    write_uint32(buf, message.kv_metadata.num_layers);
    write_uint32(buf, message.kv_metadata.num_tokens);
    write_string(buf, message.response_text);

    return buf;
}

std::unique_ptr<Message> deserialize_message(const std::vector<uint8_t> &data)
{
    if (data.empty())
    {
        Logger::error("Cannot deserialize empty message");
        return nullptr;
    }

    const uint8_t *p = data.data();
    const uint8_t *end = data.data() + data.size();

    auto msg = std::make_unique<Message>();

    uint8_t type_byte, status_byte;
    if (!read_uint8(p, end, type_byte))
    {
        Logger::error("Failed to read message type");
        return nullptr;
    }
    msg->type = static_cast<MessageType>(type_byte);

    if (!read_uint8(p, end, status_byte))
    {
        Logger::error("Failed to read status");
        return nullptr;
    }
    msg->status = static_cast<RequestStatus>(status_byte);

    if (!read_uint64(p, end, msg->transaction_id))
    {
        Logger::error("Failed to read transaction_id");
        return nullptr;
    }
    if (!read_uint64(p, end, msg->timestamp))
    {
        Logger::error("Failed to read timestamp");
        return nullptr;
    }
    if (!read_string(p, end, msg->source_node_id))
    {
        Logger::error("Failed to read source_node_id");
        return nullptr;
    }

    if (!read_request_info(p, end, msg->request_info))
    {
        Logger::error("Failed to read request_info");
        return nullptr;
    }
    if (!read_node_info(p, end, msg->node_info))
    {
        Logger::error("Failed to read node_info");
        return nullptr;
    }

    uint32_t member_count;
    if (!read_uint32(p, end, member_count))
    {
        Logger::error("Failed to read membership_list_info count");
        return nullptr;
    }
    msg->membership_list_info.resize(member_count);
    for (auto &ni : msg->membership_list_info)
    {
        if (!read_node_info(p, end, ni))
        {
            Logger::error("Failed to read membership NodeInfo");
            return nullptr;
        }
    }

    if (!read_string(p, end, msg->error_message))
    {
        Logger::error("Failed to read error_message");
        return nullptr;
    }
    if (!read_uint32(p, end, msg->kv_metadata.num_layers))
    {
        Logger::error("Failed to read kv_metadata.num_layers");
        return nullptr;
    }
    if (!read_uint32(p, end, msg->kv_metadata.num_tokens))
    {
        Logger::error("Failed to read kv_metadata.num_tokens");
        return nullptr;
    }
    if (!read_string(p, end, msg->response_text))
    {
        Logger::error("Failed to read response_text");
        return nullptr;
    }

    if (p != end)
    {
        Logger::warning("Deserialized message but " +
                        std::to_string(end - p) + " bytes remain");
    }

    return msg;
}
