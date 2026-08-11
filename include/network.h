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

    // Returns ESP32 IP address
    static String ipAddress();

    // Returns Wi-Fi signal strength (RSSI)
    static int signalStrength();

    // Returns Wi-Fi SSID
    static String ssid();

    static String macAddress();
};