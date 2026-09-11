#pragma once

#include <Arduino.h>

// Automation manages local schedule and power-outage auto-resume.
//
// CRITICAL ARCHITECTURAL GUARANTEE:
// All schedule checks, time comparisons, and auto-resume timers run
// completely LOCALLY on the ESP32 using NVS storage and the internal RTC.
// Neither WiFi, internet access, nor Firebase connectivity is required
// for irrigation schedules or auto-resume to execute safely.
namespace Automation
{
    void begin();
    void loop();

    // Auto-Resume status & controls
    bool isAutoResumePending();
    unsigned long autoResumeRemainingSeconds();
    void cancelAutoResume();
    bool autoResumeStartWasPending();

    // Schedule status & triggers
    bool scheduleStartWasPending();
    bool scheduleStopWasPending();

    // Synchronization methods called when cloud/dashboard settings arrive
    void updateSchedule(bool enabled, const String &onTime, const String &offTime);
    void updateAutoResume(bool enabled, uint8_t delayMinutes);
    void triggerAutoResume(uint8_t delayMinutes);
}
