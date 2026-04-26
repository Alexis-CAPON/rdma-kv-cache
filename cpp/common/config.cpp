#include "cpp/common/config.h"
#include <yaml-cpp/yaml.h>
#include <vector>
#include <string>

std::string Config::generate_node_id(const std::string &host, uint16_t port)
{
    std::string key = host;

    // FNV-1a 64-bit hash (no collisions in practice)
    uint64_t hash = 14695981039346656037ULL;
    for (char c : key)
    {
        hash ^= static_cast<uint8_t>(c);
        hash *= 1099511628211ULL;
    }

    return std::to_string(hash);
}

static void validate(const Config &config)
{
    if (config.node_id.empty())
        throw std::runtime_error("node_id must be positive");

    if (config.hostname.empty())
        throw std::runtime_error("host is required");

    if (config.client_socket_port <= 0 || config.client_socket_port > 65535)
        throw std::runtime_error("invalid port: " + std::to_string(config.client_socket_port));

    if (config.peers.empty())
        throw std::runtime_error("at least one peer is required");
}

Config Config::fromFile(const std::string &path)
{
    // Load and parse the file
    YAML::Node yaml = YAML::LoadFile(path); // throws if file not found

    Config config;

    config.hostname = yaml["host"].as<std::string>();
    config.client_socket_port = yaml["client_socket_port"].as<int>();
    config.server_socket_port = yaml["server_socket_port"].as<int>();
    config.node_id = generate_node_id(config.hostname, config.client_socket_port);

    // Load cluster parameters with defaults
    config.replication_factor = yaml["replication_factor"] ? yaml["replication_factor"].as<uint32_t>() : 3;
    config.number_of_keys_hashtable = yaml["number_of_keys_hashtable"] ? yaml["number_of_keys_hashtable"].as<uint32_t>() : 10000;
    config.size_local_buffer = yaml["size_local_buffer"] ? yaml["size_local_buffer"].as<size_t>() : 1024;
    config.client_event_queue_size = yaml["event_queue_size"] ? yaml["event_queue_size"].as<size_t>() : 1000;
    config.monitoring_event_queue_size = yaml["event_queue_size"] ? yaml["event_queue_size"].as<size_t>() : 1000;
    config.worker_pool_size = yaml["worker_pool_size"] ? yaml["worker_pool_size"].as<uint32_t>() : 4;

    config.orchestrator_host = yaml["orchestrator_host"].as<std::string>();
    config.orchestrator_port = yaml["orchestrator_port"].as<int>();
    config.orchestrator_id = generate_node_id(config.orchestrator_host, config.orchestrator_port);

    config.num_layers = yaml["num_layers"] ? yaml["num_layers"].as<uint32_t>() : 12;

    // Transfer backend selection
    config.use_gpu = yaml["use_gpu"] ? yaml["use_gpu"].as<bool>() : true;

    config.role = yaml["node"]["role"].as<std::string>();

    config.vnodes_number = yaml["vnodes_number"] ? yaml["vnodes_number"].as<int>() : 1;

    // ── GPUDirect RDMA Configuration ─────────────────────────────────────────
    if (yaml["node"])
    {
        const YAML::Node &node = yaml["node"];

        // RDMA parameters
        config.rdma.ib_port = node["ib_port"] ? node["ib_port"].as<int>() : 1;
        config.rdma.gid_index = node["gid_index"] ? node["gid_index"].as<int>() : 0;
        config.rdma.mtu = node["mtu"] ? node["mtu"].as<int>() : 4096;
        config.rdma.sl = node["sl"] ? node["sl"].as<int>() : 0;
        config.rdma.qp_max_send_wr = node["qp_max_send_wr"] ? node["qp_max_send_wr"].as<int>() : 64;
        config.rdma.qp_max_recv_wr = node["qp_max_recv_wr"] ? node["qp_max_recv_wr"].as<int>() : 64;
        config.rdma.qp_max_inline_data = node["qp_max_inline_data"] ? node["qp_max_inline_data"].as<int>() : 64;
        config.rdma.max_rd_atomic = node["max_rd_atomic"] ? node["max_rd_atomic"].as<int>() : 16;
        config.rdma.min_rnr_timer = node["min_rnr_timer"] ? node["min_rnr_timer"].as<int>() : 12;
        config.rdma.timeout = node["timeout"] ? node["timeout"].as<int>() : 14;
        config.rdma.retry_cnt = node["retry_cnt"] ? node["retry_cnt"].as<int>() : 7;
        config.rdma.rnr_retry = node["rnr_retry"] ? node["rnr_retry"].as<int>() : 7;
        config.rdma.cq_depth = node["cq_depth"] ? node["cq_depth"].as<int>() : 128;

        // GPU configuration
        config.gpu.enable_peer_access = node["enable_peer_access"] ? node["enable_peer_access"].as<bool>() : true;
        config.gpu.require_same_numa = node["require_same_numa"] ? node["require_same_numa"].as<bool>() : false;
    }

    // Memory configuration (can be in a separate section or with defaults)
    if (yaml["memory"])
    {
        const YAML::Node &mem = yaml["memory"];
        config.memory.kv_buffer_mb = mem["kv_buffer_mb"] ? mem["kv_buffer_mb"].as<size_t>() : 1024;
        config.memory.num_kv_chunks = mem["num_kv_chunks"] ? mem["num_kv_chunks"].as<int>() : 16;
        config.memory.chunk_size_mb = mem["chunk_size_mb"] ? mem["chunk_size_mb"].as<size_t>() : 64;
        config.memory.mr_relaxed_ordering = mem["mr_relaxed_ordering"] ? mem["mr_relaxed_ordering"].as<bool>() : true;
        config.memory.max_concurrent_requests = mem["max_concurrent_requests"] ? mem["max_concurrent_requests"].as<int>() : 8;
    }

    // Orchestrator configuration
    if (yaml["orchestrator"])
    {
        config.expected_decode_nodes = yaml["orchestrator"]["expected_decode_nodes"] ? yaml["orchestrator"]["expected_decode_nodes"].as<int>() : 1;
        config.expected_prefill_nodes = yaml["orchestrator"]["expected_prefill_nodes"] ? yaml["orchestrator"]["expected_prefill_nodes"].as<int>() : 1;
    }

    for (const auto &peer : yaml["peers"])
    {
        PeerConfig p;
        p.hostname = peer["host"].as<std::string>();
        p.client_socket_port = peer["client_socket_port"].as<uint32_t>();
        p.server_socket_port = peer["server_socket_port"].as<uint32_t>();
        p.node_id = generate_node_id(p.hostname, p.client_socket_port);
        config.peers.push_back(p);
    }

    for (const auto &prefill_node : yaml["prefill_nodes"])
    {
        PeerConfig p;
        p.hostname = prefill_node["host"].as<std::string>();
        p.client_socket_port = prefill_node["client_socket_port"].as<uint32_t>();
        p.server_socket_port = prefill_node["server_socket_port"].as<uint32_t>();
        p.node_id = generate_node_id(p.hostname, p.client_socket_port);
        config.prefill_nodes.push_back(p);
    }

    for (const auto &decode_node : yaml["decode_nodes"])
    {
        PeerConfig p;
        p.hostname = decode_node["host"].as<std::string>();
        p.client_socket_port = decode_node["client_socket_port"].as<uint32_t>();
        p.server_socket_port = decode_node["server_socket_port"].as<uint32_t>();
        p.node_id = generate_node_id(p.hostname, p.client_socket_port);
        config.decode_nodes.push_back(p);
    }

    validate(config);

    return config;
}