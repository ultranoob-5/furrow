#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

#include "notify.h"
#include "config.h"
#include "storage.h"
#include "logger.h"

namespace
{
    constexpr const char *TAG = "Notify";
    constexpr const char *WHAPI_URL = "https://gate.whapi.cloud/messages/text";

    // Minimal JSON string escaping - only what's actually needed for
    // our own alert message text (quotes, backslashes, newlines). Not
    // a general-purpose JSON encoder, just enough to not break the
    // request if a message ever contains one of these.
    String jsonEscape(const String &value)
    {
        String escaped = "";

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

namespace Notify
{
    void sendWhatsApp(const String &message)
    {
        if (!AppStorage::hasWhatsAppConfig())
            return; // no recipient set for this device - not an error, just opted out

        String phone = AppStorage::whatsAppPhone();

        WiFiClientSecure client;
        client.setInsecure(); // same tradeoff as the rest of this project's HTTPS use

        HTTPClient http;
        http.begin(client, WHAPI_URL);
        http.addHeader("Content-Type", "application/json");
        http.addHeader("Authorization", String("Bearer ") + Config::WHAPI_TOKEN);

        String body = "{\"to\":\"" + phone + "\",\"body\":\"" + jsonEscape(message) + "\"}";

        int httpCode = http.POST(body);

        if (httpCode == 200 || httpCode == 201)
        {
            Logger::info(TAG, "WhatsApp alert sent: " + message);
        }
        else
        {
            // Deliberately just a warning, never anything that could
            // propagate up and affect motor control or anything else -
            // a failed alert should only ever mean a missed message.
            String response = http.getString();
            Logger::warn(TAG, "WhatsApp alert failed (HTTP " + String(httpCode) + "): " + response);
        }

        http.end();
    }
}
