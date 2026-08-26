#pragma once

#include <Arduino.h>
#include <cstdio>

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
            unsigned char uc = (unsigned char)c;

            switch (c)
            {
                case '"':  escaped += "\\\""; break;
                case '\\': escaped += "\\\\"; break;
                case '\n': escaped += "\\n";  break;
                case '\r': escaped += "\\r";  break;
                case '\t': escaped += "\\t";  break;
                default:
                    // RFC 8259 requires escaping every control
                    // character (U+0000-U+001F), not just the common
                    // ones above - the four named cases only covered
                    // 4 of the ~32. \uXXXX is the generic escape form
                    // for anything without its own short form; an
                    // unescaped control byte here would otherwise
                    // silently produce invalid JSON, the exact failure
                    // this function exists to prevent.
                    if (uc < 0x20)
                    {
                        char buf[7];
                        snprintf(buf, sizeof(buf), "\\u%04x", uc);
                        escaped += buf;
                    }
                    else
                    {
                        escaped += c;
                    }
            }
        }

        return escaped;
    }
}
