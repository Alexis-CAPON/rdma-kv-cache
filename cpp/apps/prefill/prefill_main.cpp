#include <iostream>
#include <thread>
#include <csignal>
#include <atomic>
#include <chrono>
#include "cpp/common/messages.h"
#include "cpp/common/connection.h"
#include "cpp/common/logger.h"
#include "cpp/nodes/node.h"
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

        Node prefill_node(config);

        // Start all components
        if (!prefill_node.start())
        {
            std::cerr << "Failed to start prefill node\n";
            return 1;
        }

        Logger::info("Prefill node is running. Press Ctrl+C to stop.");

        while (running.load())
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        // Graceful shutdown
        Logger::info("Shutting down...");
        prefill_node.shutdown();

        Logger::info("Orchestrator engine shut down cleanly");
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