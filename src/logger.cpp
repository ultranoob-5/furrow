#include "logger.h"

namespace
{
    const char *levelLabel(LogLevel level)
    {
        switch (level)
        {
            case LogLevel::INFO:  return "INFO ";
            case LogLevel::WARN:  return "WARN ";
            case LogLevel::ERROR: return "ERROR";
        }

        return "?????";
    }
}

namespace Logger
{
    void log(LogLevel level, const char *tag, const String &message)
    {
        Serial.printf("[%8lu] %s [%s] %s\n",
                      static_cast<unsigned long>(millis()),
                      levelLabel(level),
                      tag,
                      message.c_str());
    }
}
