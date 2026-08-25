#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

#include "notify.h"
#include "config.h"
#include "storage.h"
#include "logger.h"
#include "json_util.h"

namespace
{
    constexpr const char *TAG = "Notify";
    constexpr const char *WHAPI_URL = "https://gate.whapi.cloud/messages/text";
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

        String body = "{\"to\":\"" + JsonUtil::escape(phone) + "\",\"body\":\"" + JsonUtil::escape(message) + "\"}";

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
