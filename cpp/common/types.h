#pragma once
#include <string>
#include <vector>
#include <cstdint>

struct QMap
{
    std::string peer_node_id; // Who this QP is for

    // Local QP info (this node's side of connection)
    uint32_t qp_num;
    uint16_t lid;
    uint8_t gid[16];
    uint32_t psn;
};

// Node information
struct NodeInfo
{
    std::string node_id;
    NodeRole role;
    std::string ip_address;
    uint16_t tcp_port;  // For orchestrator communication
    uint16_t vllm_port; // vLLM HTTP port
    bool is_healthy;

    int gpu_lid; // For RDMA transfers

    uint64_t rkey;
    uint64_t gpu_base_addr;
    uint64_t lkey;
    size_t memory_pool_size;

    std::vector<QMap> qmaps; // For RDMA connection info exchange

    // Example:
    // prefill-01's qmaps: [
    //   {peer_id: "decode-01", qp_num: 100, lid: 1, ...},
    //   {peer_id: "decode-02", qp_num: 101, lid: 1, ...},
    //   {peer_id: "decode-03", qp_num: 102, lid: 1, ...}
    // ]
};

// KV cache metadata
struct KVLayerMetadata
{
    std::string request_id;
    std::string layer_name;
    int layer_id;
    std::vector<int> block_ids;
    size_t data_size;
    void *gpu_ptr;
    uint64_t timestamp;
};

// RDMA connection info for QP exchange
struct QpInfo
{
    uint32_t qp_num;
    uint16_t lid;
    uint8_t gid[16];
    uint32_t psn; // Packet sequence number
};

enum class NodeRole
{
    ORCHESTRATOR,
    PREFILL,
    DECODE
};

enum class MessageType
{
    // CLIENT -> Orchestrator
    CLIENT_REQUEST,  // For LLM request
    CLIENT_RESPONSE, // For LLM response
    CLIENT_ERROR,    // For LLM errors

    RDMA_PROCESS_REGISTRATION, // // Used by orchestrator to process RDMA message received from prefill/decode node

    // Orchestrator -> Node
    ASSIGN_REQUEST,
    // HEALTH_CHECK,

    // Node -> Orchestrator
    BROADCAST_MEMBER_INFO,      // Used by prefill/decode node to handle the QP map and all the RDMA info by the orchestrator and connect all QP between all the node and then send confirmation to orchestator
    RDMA_REGISTRATION_COMPLETE, // Used by a node to signal completion of RDMA registration by a node
    PREFILL_COMPLETE,           // Used by prefill node to inform orchestrator that prefill phase is complete
    DECODE_COMPLETE,            // Also useby prefill node to signal completion of prefill phase to decode node
    REQUEST_FAILED,

    RDMA_READY, // Used by prefill/decode node to signal to orchestrator that we are ready for RDMA communication

    // Node -> Node (via orchestrator)
    TRANSFER_READY
};

struct RequestInfo
{
    std::string request_id;
    std::string prompt;
    int max_tokens;
    std::string prefill_node_id;
    std::string decode_node_id;
    uint64_t timestamp_created;
    uint64_t timestamp_prefill_start;
    uint64_t timestamp_prefill_done;
    uint64_t timestamp_decode_start;
    uint64_t timestamp_decode_done;
};

enum class RequestStatus
{
    PENDING,
    PREFILLING,
    TRANSFERRING,
    DECODING,
    COMPLETED,
    FAILED,
    ERROR
};
