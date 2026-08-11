#pragma once

#include <Arduino.h>

// Storage persists WiFi credentials AND per-device identity (name,
// owner email) across reboots using the ESP32's NVS flash (via the
// Preferences library). This is what makes one firmware binary work
// for any device/owner - everything that differs between installs is
// set at runtime via the captive portal, not compiled in.
namespace AppStorage
{
    void begin();

    bool hasWifiCredentials();
    String wifiSSID();
    String wifiPassword();
    void setWifiCredentials(const String &ssid, const String &password);

    // Wipes stored WiFi credentials, forcing provisioning mode on next boot.
    void clearWifiCredentials();

    // True once both device name and owner email have been set.
    bool hasDeviceConfig();
    String deviceName();
    String ownerEmail();
    void setDeviceConfig(const String &name, const String &owner);

    // WhatsApp alerts (via Whapi.cloud) - entirely optional, unlike
    // WiFi/device config above. Provisioning completes fine without
    // this ever being set; sendWhatsApp() (see notify.h) just silently
    // does nothing until it is. Only the RECIPIENT phone number lives
    // here - the sender token is a shared credential in secrets.h,
    // the same for every device, not something set per-device.
    bool hasWhatsAppConfig();
    String whatsAppPhone();
    void setWhatsAppConfig(const String &phone);
}
