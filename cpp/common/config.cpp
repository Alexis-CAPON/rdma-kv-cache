#include "node/config.h"
#include <yaml-cpp/yaml.h>

uint64_t Config::generate_node_id(const std::string &host, uint16_t port)
{
    std::string key = host;

    // FNV-1a 64-bit hash (no collisions in practice)
    uint64_t hash = 14695981039346656037ULL;
    for (char c : key)
    {
        hash ^= static_cast<uint8_t>(c);
        hash *= 1099511628211ULL;
    }

    return hash;
}

static void validate(const Config &config)
{
    if (config.node_id <= 0)
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

    config.monitoring_host = yaml["monitoring_host"].as<std::string>();
    config.monitoring_port = yaml["monitoring_port"].as<int>();

    config.role = yaml["node"]["role"].as<std::string>();

    config.vnodes_number = yaml["vnodes_number"].as<int>();

    for (const auto &peer : yaml["peers"])
    {
        PeerConfig p;
        p.hostname = peer["host"].as<std::string>();
        p.client_socket_port = peer["client_socket_port"].as<uint32_t>();
        p.server_socket_port = peer["server_socket_port"].as<uint32_t>();
        p.node_id = generate_node_id(p.hostname, p.client_socket_port);
        config.peers.push_back(p);
    }

    validate(config);

    return config;
}