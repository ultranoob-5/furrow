#pragma once

#include <Arduino.h>

class Network
{
public:
    // Attempts to connect using stored credentials. Returns true if
    // connected within the timeout, false if it should fall back to
    // provisioning mode instead.
    static bool begin();

    // Keep Wi-Fi alive (call in loop())
    static void loop();

    // Returns true if connected
    static bool isConnected();

    // Returns true exactly once, right after Wi-Fi reconnects following
    // a disconnect. Clears itself once read, so callers should call this
    // once per loop() and act on it immediately.
    static bool consumeReconnectEvent();

    // Valid to read right after consumeReconnectEvent() returns true -
    // how long the most recent disconnect lasted, in milliseconds. Only
    // covers WiFi/network drops while the device stayed powered - a
    // genuine power loss (device fully off) can't be measured this way,
    // since nothing runs to track time while there's no power at all.
    static unsigned long lastDisconnectDurationMs();

    // Returns ESP32 IP address
    static String ipAddress();

    // Returns Wi-Fi signal strength (RSSI)
    static int signalStrength();

    // Returns Wi-Fi SSID
    static String ssid();

    static String macAddress();
};