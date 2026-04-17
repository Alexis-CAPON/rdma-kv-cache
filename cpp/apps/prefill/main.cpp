#include <iostream>
#include <thread>
#include <csignal>
#include <atomic>
#include <chrono>
#include "common/messages.h"
#include "common/connection.h"
#include "common/logger.h"
#include "node/node.h"
#include <yaml-cpp/yaml.h>

using namespace std;

// Global flag for graceful shutdown
extern std::atomic<bool> running;

int main(int argc, char *argv[])
{
    // Reading arguments
    std::string config_path;
    for (int i = 0; i < argc; ++i)
    {
        std::string arg = argv[i];

        if (arg == "--config" && i + 1 < argc)
        {
            config_path = argv[++i];
            Logger::info("Using config file: " + config_path);
            // Load config from file
        }
    }

    if (config_path.empty())
    {
        std::cerr << "Missing --config argument\n";
        return 1;
    }

    // Set up signal handlers
    std::signal(SIGINT, [](int)
                {
        Logger::info("Received shutdown signal");
        running.store(false); });
    std::signal(SIGTERM, [](int)
                {
        Logger::info("Received termination signal");
        running.store(false); });

    try
    {
        // Initialize node
        Config config = Config::fromFile(config_path);

        Node server_node(config);

        // Start all components
        if (!server_node.start())
        {
            std::cerr << "Failed to start server node\n";
            return 1;
        }

        Logger::info("RHT node is running. Press Ctrl+C to stop.");

        while (running.load())
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        // Graceful shutdown
        Logger::info("Shutting down...");
        server_node.shutdown();

        Logger::info("RHT node shut down cleanly");
        return 0;
    }
    catch (const YAML::Exception &e)
    {
        std::cerr << "YAML parse error: " << e.what() << "\n";
        return 1;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Startup error: " << e.what() << "\n";
        return 1;
    }
}