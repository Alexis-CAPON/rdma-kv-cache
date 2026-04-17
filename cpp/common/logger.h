#pragma once
#include <string>

class Logger
{
public:
    enum class LogLevel
    {
        DEBUG,
        INFO,
        WARNING,
        ERROR
    };

    static void debug(const std::string &message);
    static void info(const std::string &message);
    static void warning(const std::string &message);
    static void error(const std::string &message);

    static void set_level(LogLevel level);

private:
    static LogLevel current_level_;
    static void log(const std::string &message, LogLevel level);
};