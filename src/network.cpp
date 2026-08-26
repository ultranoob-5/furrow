#include "network.h"
#include "config.h"
#include "logger.h"
#include "storage.h"

#include <Arduino.h>
#include <WiFi.h>

namespace
{
    unsigned long lastReconnectAttempt = 0;
    bool wasConnected = false;
    bool reconnectEvent = false;

    unsigned long disconnectedAt = 0;
    unsigned long lastDisconnectDuration = 0;

    constexpr const char *TAG = "Network";
    constexpr unsigned long CONNECT_TIMEOUT_MS = 15000;
}

bool Network::begin()
{
    String ssid = AppStorage::wifiSSID();
    String password = AppStorage::wifiPassword();

    // Retries a few times (each with the existing 15s timeout) before
    // giving up entirely, rather than falling back to full
    // reprovisioning after a single attempt. A transient issue right
    // at boot - the router still rebooting after a shared power blip,
    // for instance - shouldn't wipe perfectly good stored credentials
    // and strand the device waiting for someone to physically visit
    // and reconfigure it through the captive portal. This is the same
    // resilience philosophy Network::loop() already applies once
    // running (retries indefinitely, every 5s) - this just extends it
    // to cover the boot-time connection too, instead of only what
    // happens after the first one succeeds.
    constexpr int MAX_CONNECT_ATTEMPTS = 3;

    for (int attempt = 1; attempt <= MAX_CONNECT_ATTEMPTS; attempt++)
    {
        Logger::info(TAG, "Connecting to WiFi \"" + ssid + "\" (attempt " +
                           String(attempt) + "/" + String(MAX_CONNECT_ATTEMPTS) + ")...");

        WiFi.mode(WIFI_STA);
        WiFi.begin(ssid.c_str(), password.c_str());

        unsigned long start = millis();

        while (WiFi.status() != WL_CONNECTED)
        {
            if (millis() - start >= CONNECT_TIMEOUT_MS)
                break;

            delay(300);
        }

        if (WiFi.status() == WL_CONNECTED)
        {
            wasConnected = true;

            Logger::info(TAG, "WiFi connected - IP: " + WiFi.localIP().toString() +
                               ", RSSI: " + String(WiFi.RSSI()) + " dBm");

            return true;
        }

        Logger::warn(TAG, "Connect attempt " + String(attempt) + " timed out");

        WiFi.disconnect();
    }

    Logger::warn(TAG, "All " + String(MAX_CONNECT_ATTEMPTS) + " connect attempts failed");

    return false;
}

void Network::loop()
{
    bool connected = (WiFi.status() == WL_CONNECTED);

    if (connected && !wasConnected)
    {
        lastDisconnectDuration = (disconnectedAt > 0) ? (millis() - disconnectedAt) : 0;

        Logger::info(TAG, "WiFi reconnected - was down for " + String(lastDisconnectDuration / 1000) + "s");
        reconnectEvent = true;
    }
    else if (!connected && wasConnected)
    {
        disconnectedAt = millis();

        Logger::warn(TAG, "WiFi lost");
    }

    wasConnected = connected;

    if (connected)
        return;

    if (millis() - lastReconnectAttempt < 5000)
        return;

    lastReconnectAttempt = millis();

    Logger::info(TAG, "Reconnecting...");

    WiFi.disconnect();
    WiFi.begin(AppStorage::wifiSSID().c_str(), AppStorage::wifiPassword().c_str());
}

bool Network::isConnected()
{
    return WiFi.status() == WL_CONNECTED;
}

bool Network::consumeReconnectEvent()
{
    if (!reconnectEvent)
        return false;

    reconnectEvent = false;
    return true;
}

unsigned long Network::lastDisconnectDurationMs()
{
    return lastDisconnectDuration;
}

String Network::ipAddress()
{
    return WiFi.localIP().toString();
}

int Network::signalStrength()
{
    return WiFi.RSSI();
}

String Network::ssid()
{
    return WiFi.SSID();
}

String Network::macAddress()
{
    return WiFi.macAddress();
}
