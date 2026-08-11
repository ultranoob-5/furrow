#pragma once

#include <Arduino.h>

// Replaces scattered Serial.print/println/printf calls with a single,
// consistent format: a timestamp (ms since boot), a level, a module tag,
// and the message. Example line:
//
//   [   4213] INFO  [Cloud] Listening for remote commands
//
// This is also the one place that would need to change if logs ever need
// to go somewhere other than Serial (e.g. a remote log buffer) - callers
// never touch Serial directly.
enum class LogLevel
{
    INFO,
    WARN,
    ERROR
};

namespace Logger
{
    void log(LogLevel level, const char *tag, const String &message);

    inline void info(const char *tag, const String &message)  { log(LogLevel::INFO,  tag, message); }
    inline void warn(const char *tag, const String &message)  { log(LogLevel::WARN,  tag, message); }
    inline void error(const char *tag, const String &message) { log(LogLevel::ERROR, tag, message); }
}
