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

    Logger::info(TAG, "Connecting to WiFi \"" + ssid + "\"...");

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), password.c_str());

    unsigned long start = millis();

    while (WiFi.status() != WL_CONNECTED)
    {
        if (millis() - start >= CONNECT_TIMEOUT_MS)
        {
            Logger::warn(TAG, "Connect timed out");
            return false;
        }

        delay(300);
    }

    wasConnected = true;

    Logger::info(TAG, "WiFi connected - IP: " + WiFi.localIP().toString() +
                       ", RSSI: " + String(WiFi.RSSI()) + " dBm");

    return true;
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
