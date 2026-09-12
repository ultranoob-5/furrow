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

    // Fast initial connect attempt (8s) so the motor controller boots and
    // starts local protection/control immediately without waiting 45s if
    // the farm router is still booting after a power cut. If this times out,
    // boot continues locally and Network::loop() reconnects in the background.
    constexpr unsigned long BOOT_CONNECT_TIMEOUT_MS = 8000;

    Logger::info(TAG, "Connecting to WiFi \"" + ssid + "\"...");

    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(ssid.c_str(), password.c_str());

    unsigned long start = millis();

    while (WiFi.status() != WL_CONNECTED)
    {
        if (millis() - start >= BOOT_CONNECT_TIMEOUT_MS)
            break;

        delay(200);
    }

    if (WiFi.status() == WL_CONNECTED)
    {
        wasConnected = true;

        Logger::info(TAG, "WiFi connected - IP: " + WiFi.localIP().toString() +
                           ", RSSI: " + String(WiFi.RSSI()) + " dBm");

        // Synchronize with atomic clocks via NTP in IST (UTC+5:30, 19800 seconds).
        // No DST offset needed for India.
        configTime(19800, 0, "pool.ntp.org", "time.google.com");

        return true;
    }

    Logger::warn(TAG, "Initial WiFi connect timed out - continuing boot in local mode (will reconnect in background)");
    return false;
}

void Network::loop()
{
    bool connected = (WiFi.status() == WL_CONNECTED);

    if (connected && !wasConnected)
    {
        lastDisconnectDuration = (disconnectedAt > 0) ? (millis() - disconnectedAt) : 0;

        Logger::info(TAG, "WiFi connected - IP: " + WiFi.localIP().toString() +
                           ", RSSI: " + String(WiFi.RSSI()) + " dBm" +
                           (disconnectedAt > 0 ? " (was down for " + String(lastDisconnectDuration / 1000) + "s)" : ""));
        configTime(19800, 0, "pool.ntp.org", "time.google.com");
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

bool Network::hasInternet()
{
    if (!isConnected())
        return false;

    IPAddress ip;
    if (WiFi.hostByName("pool.ntp.org", ip) == 1 && ip != IPAddress(0, 0, 0, 0))
        return true;

    return (WiFi.hostByName("time.google.com", ip) == 1 && ip != IPAddress(0, 0, 0, 0));
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
