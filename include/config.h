#pragma once

#include "secrets.h"

namespace Config
{
    // Relay Pins
    constexpr uint8_t RELAY_START = 26;
    constexpr uint8_t RELAY_STOP  = 27;

    // Current sensor
    // ADC1 GPIO34 is input-only and remains usable while Wi-Fi is active.
    constexpr uint8_t CURRENT_ADC_PIN = 34;

    // Timing
    constexpr uint32_t BUTTON_PRESS_TIME = 500;

    // Firmware version - bump this with every meaningful change.
    // MAJOR.MINOR.PATCH: PATCH for fixes, MINOR for new features
    // (backwards compatible), MAJOR reserved for a genuine 1.0 release.
    constexpr const char* FIRMWARE_VERSION = "1.2.1";

    // WiFi credentials, device name, and owner email are NOT compiled
    // in anymore - see provisioning.h. They're set at runtime via the
    // SoftAP captive portal and persisted in Storage (NVS). This is
    // what makes one firmware binary work for every device/owner.

    // Everything actually secret (Firebase project details, the
    // firmware's device account, the WhatsApp sender token) now lives
    // in secrets.h, which is gitignored - never committed. See
    // secrets.example.h for the template, and SETUP.md for how CI
    // generates the real one from GitHub Actions secrets at build time.
}