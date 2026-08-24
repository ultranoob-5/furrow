#include <Arduino.h>

#include "storage.h"
#include "provisioning.h"
#include "network.h"
#include "device.h"
#include "motor.h"
#include "cloud.h"
#include "notify.h"

namespace
{
    bool previousMotorRunning = false;
    unsigned long lastCurrentSerialMs = 0;
    constexpr unsigned long CURRENT_SERIAL_INTERVAL_MS = 10000;

    // GPIO0 (the BOOT button) is also the ESP32's own boot-mode
    // strapping pin - the ROM bootloader samples it at the instant of
    // power-on/reset, BEFORE any of this firmware ever runs. Holding
    // it down through power-on makes the ROM enter USB download mode
    // instead of ever starting this code at all (that's the
    // "waiting for download" state, not a bug in this firmware - it's
    // the chip never reaching main() in the first place).
    //
    // Because of that, this deliberately does NOT check the button
    // until well after normal boot has already happened, and waits
    // for a fresh press-and-hold rather than requiring it already be
    // held - the ROM's sampling window is over by the time this runs,
    // so it's safe to read GPIO0 here regardless of what it did before.
    constexpr uint8_t PROVISION_BUTTON_PIN = 0;
    constexpr unsigned long PROVISION_HOLD_MS = 3000;
    constexpr unsigned long PROVISION_WINDOW_MS = 5000;

    bool bootHeldForProvisioning()
    {
        pinMode(PROVISION_BUTTON_PIN, INPUT_PULLUP);

        Serial.println();
        Serial.println("Hold BOOT now for 3s to reconfigure WiFi (5s window)...");

        unsigned long windowStart = millis();
        unsigned long pressStart = 0;
        bool pressing = false;

        while (millis() - windowStart < PROVISION_WINDOW_MS)
        {
            bool held = digitalRead(PROVISION_BUTTON_PIN) == LOW;

            if (held && !pressing)
            {
                pressing = true;
                pressStart = millis();
            }
            else if (!held && pressing)
            {
                pressing = false; // released early - must press again
            }

            if (pressing && (millis() - pressStart >= PROVISION_HOLD_MS))
            {
                Serial.println("BOOT held - reconfiguring WiFi...");
                return true;
            }

            delay(20);
        }

        return false;
    }
}

void setup()
{
    Serial.begin(115200);

    delay(1000);

    AppStorage::begin();

    bool forceProvisioning = bootHeldForProvisioning();

    if (forceProvisioning || !AppStorage::hasWifiCredentials() || !AppStorage::hasDeviceConfig())
    {
        Provisioning::run(); // blocks, restarts the device once done
    }

    if (!Network::begin())
    {
        // Stored credentials didn't work (wrong password, network gone,
        // moved locations) - fall back to provisioning rather than
        // retrying a connection that clearly isn't going to succeed.
        Provisioning::run();
    }

    device.begin();

    motor.begin();

    cloud.begin();

    previousMotorRunning = motor.isRunning();

    Serial.println();
    Serial.println("System Ready");

    cloud.publishDevice();
    cloud.publishMotor();

    Notify::sendWhatsApp("\u2705 " + device.name() + " is online (firmware " + device.firmware() + ")");
}

void loop()
{
    Network::loop();

    device.loop();

    motor.update();

    // Motor state now comes from the CT current sensor, so this catches
    // remote commands, physical starter-button operation, and real
    // motor stops/trips.
    bool runningNow = motor.isRunning();

    // Print the measured RMS current every 10 seconds for local testing.
    // The current value is intentionally NOT published to Firebase.
    unsigned long nowMs = millis();
    if (nowMs - lastCurrentSerialMs >= CURRENT_SERIAL_INTERVAL_MS)
    {
        lastCurrentSerialMs = nowMs;
        Serial.printf("[Current] %.2f A RMS | Motor: %s\n",
                      motor.currentAmps(),
                      runningNow ? "RUNNING" : "OFF");
    }

    if (runningNow != previousMotorRunning)
    {
        previousMotorRunning = runningNow;

        Serial.println(runningNow ? "Motor is now RUNNING" : "Motor is now OFF");

        cloud.publishMotor();
    }

    cloud.loop();

    delay(10);
}
