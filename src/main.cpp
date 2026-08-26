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

    // False until the first real CT reading completes - motor.begin()
    // hasn't taken any actual sample yet (CurrentSensor::begin() just
    // initializes state, defaulting to not-running), so seeding
    // previousMotorRunning immediately after it would be wrong
    // whenever the motor is actually already running across a reboot
    // (most plausible after a deliberate remote restart or firmware
    // update - a shared-circuit power loss would normally stop the
    // motor too, not just this device). Without this, that first real
    // reading gets misread as a fresh transition and incorrectly
    // tagged "manual" in the push notification, even though nothing
    // was actually pressed - the motor was just running the whole
    // time, through the reboot.
    bool baselineEstablished = false;
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

    // previousMotorRunning is NOT seeded here - see baselineEstablished's
    // comment above. loop() establishes it from the first real CT
    // reading instead.

    Serial.println();
    Serial.println("System Ready");

    cloud.publishDevice();

    // publishMotor() is deliberately NOT called here - motor.isRunning()
    // right now is still just CurrentSensor's default (not-running),
    // since no real reading exists yet. Publishing that would tell
    // Firebase "OFF" even if the motor is genuinely running across this
    // reboot, wrong for however long the first real reading actually
    // takes (see current_sensor.h's SAMPLES comment - no longer a fixed
    // ~400ms guarantee). loop() publishes the real state itself, the
    // moment baselineEstablished actually becomes true.

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

    if (!baselineEstablished)
    {
        // Wait for the first real CT reading before comparing anything -
        // motor.isRunning() before that point is just CurrentSensor's
        // default (not-running), not a real measurement. Establishing
        // the baseline here, rather than treating this first real
        // reading as a "transition," avoids a false publish/alert for
        // whatever the actual pre-boot state genuinely was.
        if (motor.hasReading())
        {
            previousMotorRunning = runningNow;
            baselineEstablished = true;

            // The one and only place the FIRST motor-state publish
            // happens - setup() deliberately skips it (see its own
            // comment), and Cloud::loop()'s heartbeat/handlePendingCommand's
            // republish are both gated behind motor.hasReading() too
            // (src/cloud.cpp), so nothing else can publish a guess
            // before this point either. No startedVia tag - this
            // isn't a transition, it's the first time the real state
            // is known at all.
            cloud.publishMotor();
        }
    }
    else if (runningNow != previousMotorRunning)
    {
        previousMotorRunning = runningNow;

        Serial.println(runningNow ? "Motor is now RUNNING" : "Motor is now OFF");

        // startedVia only ever applies to a transition into RUNNING -
        // see cloud.h's comment on remoteStartWasPending() for why a
        // stop can't be inferred the same safe way (power loss, a
        // trip, or an overload can all stop a motor with no remote
        // command involved at all, so "no remote command pending"
        // doesn't mean "manual" the way it does for a start).
        if (runningNow)
            cloud.publishMotor(cloud.remoteStartWasPending() ? "remote" : "manual");
        else
            cloud.publishMotor();
    }

    cloud.loop();

    // Was delay(10) - reduced specifically because of a real, confirmed
    // problem: CurrentSensor::update() takes at most one ADC sample per
    // call, and the 1ms rate-limiting inside it (SAMPLE_INTERVAL_US) only
    // works if update() is actually called faster than that. At a
    // delay(10)-dominated ~12ms loop cadence, it wasn't - every call took
    // exactly one sample regardless of the 1ms target, so a "400 samples
    // @ 1ms = 400ms" window actually took roughly 400 * 12ms = ~4.8s (a
    // 12x slowdown), and RUN_CONFIRM_WINDOWS's "3 windows = ~1.2s" became
    // more like 14+ seconds in reality. Confirmed by simulating the exact
    // scheduling logic, not just inspected. delay(1) still yields to
    // FreeRTOS (unlike removing the delay entirely, which risks starving
    // the WiFi stack or the watchdog), while letting loop() cycle enough
    // times per millisecond for CurrentSensor's existing 1ms rate-limit
    // to actually become the binding constraint again, rather than the
    // outer loop cadence being the real bottleneck it was quietly acting
    // as. Doesn't guarantee hitting the intended rate exactly - the rest
    // of loop() (cloud.loop() especially, during real network I/O) can
    // still occasionally take longer than 1ms - see current_sensor.cpp's
    // window-duration log for what's actually being achieved on real
    // hardware, which this sandbox has no way to measure directly.
    delay(1);
}
