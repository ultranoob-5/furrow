#pragma once

#include <Arduino.h>

// Minimal JSON string escaping - covers exactly what this project's
// hand-built JSON payloads need (quotes, backslashes, newlines), not a
// general-purpose JSON encoder. Anything that builds a JSON body by
// string concatenation instead of a real JSON library should route any
// person-typed value (device name, owner email, WhatsApp recipient,
// alert text, etc.) through this first - otherwise a stray '"' or '\'
// in what someone typed during provisioning breaks the JSON and the
// write/request silently fails instead of just containing that value.
namespace JsonUtil
{
    inline String escape(const String &value)
    {
        String escaped = "";
        escaped.reserve(value.length());

        for (size_t i = 0; i < value.length(); i++)
        {
            char c = value.charAt(i);

            switch (c)
            {
                case '"':  escaped += "\\\""; break;
                case '\\': escaped += "\\\\"; break;
                case '\n': escaped += "\\n";  break;
                case '\r': escaped += "\\r";  break;
                default:   escaped += c;
            }
        }

        return escaped;
    }
}
