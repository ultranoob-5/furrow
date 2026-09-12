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
    void setDeviceName(const String &name);

    // WhatsApp alerts (via Whapi.cloud) - entirely optional, unlike
    // WiFi/device config above. Provisioning completes fine without
    // this ever being set; sendWhatsApp() (see notify.h) just silently
    // does nothing until it is. Only the RECIPIENT phone number lives
    // here - the sender token is a shared credential in secrets.h,
    // the same for every device, not something set per-device.
    bool hasWhatsAppConfig();
    String whatsAppPhone();
    void setWhatsAppConfig(const String &phone);

    // Development device flag - when true, disables CT current sensor feedback
    // so Start/Stop commands immediately update motor state for bench testing
    // without requiring physical CT hardware or a motor.
    bool isDevelopmentDevice();
    void setDevelopmentDevice(bool enabled);

    // Schedule configuration persisted in NVS so the device can execute
    // scheduled motor starts and stops even when WiFi/internet is down.
    bool scheduleEnabled();
    String scheduleOnTime();
    String scheduleOffTime();
    void setScheduleConfig(bool enabled, const String &onTime, const String &offTime);

    // Auto-Resume configuration persisted in NVS so the device can resume
    // operation after a power outage without depending on Firebase connectivity.
    bool autoResumeEnabled();
    uint8_t autoResumeDelayMinutes();
    void setAutoResumeConfig(bool enabled, uint8_t delayMinutes);

    // Motor state persisted across power outages. Updated on confirmed
    // motor state transitions (RUNNING/OFF). When power is restored,
    // the firmware checks this locally to know if the motor was running.
    String lastMotorState();
    void setLastMotorState(const String &state);

    // Wipes WiFi credentials, device name/owner, WhatsApp config,
    // and automation preferences - returning the device to factory defaults.
    void factoryReset();
}

