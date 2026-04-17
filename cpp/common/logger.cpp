#include "common/logger.h"
#include <iostream>
#include <ctime>
#include <mutex>

Logger::LogLevel Logger::current_level_ = Logger::LogLevel::DEBUG;

void Logger::set_level(LogLevel level)
{
    current_level_ = level;
}

void Logger::log(const std::string &message, LogLevel level)
{
    if (level < current_level_)
    {
        return;
    }

    const char *level_str = "";
    switch (level)
    {
    case LogLevel::DEBUG:
        level_str = "DEBUG";
        break;
    case LogLevel::INFO:
        level_str = "INFO";
        break;
    case LogLevel::WARNING:
        level_str = "WARNING";
        break;
    case LogLevel::ERROR:
        level_str = "ERROR";
        break;
    }

    // localtime() is not thread-safe. Use localtime_r() and serialize output.
    time_t now = time(nullptr);
    std::tm local_time;
    localtime_r(&now, &local_time);

    char time_buf[20];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &local_time);

    static std::mutex log_mutex;
    std::lock_guard<std::mutex> lock(log_mutex);
    std::cout << "[" << time_buf << "] [" << level_str << "] " << message << std::endl;
}

void Logger::debug(const std::string &message)
{
    log(message, LogLevel::DEBUG);
}

void Logger::info(const std::string &message)
{
    log(message, LogLevel::INFO);
}

void Logger::warning(const std::string &message)
{
    log(message, LogLevel::WARNING);
}

void Logger::error(const std::string &message)
{
    log(message, LogLevel::ERROR);
}
