#pragma once
#include <string>
#include <vector>
#include <cstdint>

struct PeerConfig
{
    uint64_t node_id;
    std::string hostname;
    uint32_t client_socket_port;
    uint32_t server_socket_port;
};

class Config
{
public:
    static uint64_t generate_node_id(const std::string &host, uint16_t port);
    uint64_t node_id;
    uint32_t client_socket_port;
    uint32_t server_socket_port;
    std::string hostname;
    uint32_t replication_factor;
    uint32_t vnodes_number;
    uint32_t number_of_keys_hashtable;
    size_t size_local_buffer;
    size_t client_event_queue_size;
    size_t monitoring_event_queue_size;
    uint32_t worker_pool_size;
    uint32_t quorum_read_requirement;
    uint32_t quorum_write_requirement;
    std::string monitoring_host;
    uint32_t monitoring_port;

    std::string role;

    uint32_t orchestrator_event_queue_size;

    int expected_decode_nodes;
    int expected_prefill_nodes;

    std::vector<PeerConfig>
        peers;

    static Config fromFile(const std::string &path);
    static Config fromArgs(int argc, char *argv[]);
};