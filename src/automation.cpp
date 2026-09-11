#include "automation.h"
#include "storage.h"
#include "motor.h"
#include "logger.h"

#include <time.h>

namespace
{
    constexpr const char *TAG = "Automation";

    // Auto-resume state
    bool autoResumePending = false;
    bool autoResumeStartPending = false;
    unsigned long autoResumeDueMs = 0;

    // Schedule state
    bool scheduleStartPending = false;
    bool scheduleStopPending = false;
    unsigned long lastScheduleCheckMs = 0;
    String lastOnFiredDate = "";
    String lastOffFiredDate = "";
}

namespace Automation
{
    void begin()
    {
        bool enabled = AppStorage::autoResumeEnabled();
        String lastState = AppStorage::lastMotorState();
        uint8_t delayMin = AppStorage::autoResumeDelayMinutes();

        // If auto-resume is enabled and the motor was RUNNING when power was lost,
        // schedule a local start after the configured delay.
        if (enabled && lastState == "RUNNING")
        {
            Logger::warn(TAG, "Auto-resume: motor was RUNNING before power loss. Scheduling start in " +
                               String(delayMin) + " minute(s)");
            autoResumePending = true;
            autoResumeDueMs = millis() + (delayMin * 60000UL);
            // Clear last state from NVS so this outage trigger only fires once
            AppStorage::setLastMotorState("");
        }
        else
        {
            // Clear stale state if any
            if (lastState.length() > 0)
                AppStorage::setLastMotorState("");
        }

        if (AppStorage::scheduleEnabled())
        {
            Logger::info(TAG, "Schedule active - ON: " + AppStorage::scheduleOnTime() +
                               ", OFF: " + AppStorage::scheduleOffTime());
        }
    }

    void loop()
    {
        // 1. Check Auto-Resume countdown
        if (autoResumePending)
        {
            if (motor.isRunning())
            {
                Logger::info(TAG, "Auto-resume cancelled - motor already running");
                autoResumePending = false;
            }
            else if ((long)(millis() - autoResumeDueMs) >= 0)
            {
                autoResumePending = false;
                autoResumeStartPending = true;
                Logger::warn(TAG, "Auto-resume delay elapsed - starting motor locally");
                motor.start();
            }
        }

        // 2. Check Irrigation Schedule (runs at 1Hz cadence)
        if (!AppStorage::scheduleEnabled())
            return;

        if (millis() - lastScheduleCheckMs < 1000)
            return;
        lastScheduleCheckMs = millis();

        time_t now = time(nullptr);
        if (now < 1704067200) // Valid time after 2024-01-01 (clock synced)
            return;

        struct tm timeinfo;
        if (!localtime_r(&now, &timeinfo))
            return;

        char nowTime[6];
        snprintf(nowTime, sizeof(nowTime), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);

        char nowDate[11];
        snprintf(nowDate, sizeof(nowDate), "%04d-%02d-%02d",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);

        String onTime = AppStorage::scheduleOnTime();
        String offTime = AppStorage::scheduleOffTime();

        // Safety guards against misconfiguration
        if (onTime == offTime || onTime.length() != 5 || offTime.length() != 5)
            return;

        bool onDue = (onTime == nowTime && lastOnFiredDate != nowDate);
        bool offDue = (offTime == nowTime && lastOffFiredDate != nowDate);

        if (onDue)
        {
            lastOnFiredDate = nowDate;
            if (!motor.isRunning())
            {
                Logger::warn(TAG, "Schedule: turning ON at " + String(nowTime));
                scheduleStartPending = true;
                scheduleStopPending = false;
                motor.start();
            }
            else
            {
                Logger::info(TAG, "Schedule: start time reached but motor already RUNNING");
            }
        }

        if (offDue)
        {
            lastOffFiredDate = nowDate;
            if (motor.isRunning())
            {
                Logger::warn(TAG, "Schedule: turning OFF at " + String(nowTime));
                scheduleStopPending = true;
                scheduleStartPending = false;
                motor.stop();
            }
            else
            {
                Logger::info(TAG, "Schedule: stop time reached but motor already OFF");
            }
        }
    }

    bool isAutoResumePending()
    {
        return autoResumePending;
    }

    unsigned long autoResumeRemainingSeconds()
    {
        if (!autoResumePending)
            return 0;
        long diff = (long)(autoResumeDueMs - millis());
        return (diff > 0) ? (diff / 1000) : 0;
    }

    void cancelAutoResume()
    {
        if (autoResumePending)
        {
            autoResumePending = false;
            Logger::info(TAG, "Auto-resume cancelled");
        }
    }

    bool autoResumeStartWasPending()
    {
        if (!autoResumeStartPending)
            return false;
        autoResumeStartPending = false;
        return true;
    }

    bool scheduleStartWasPending()
    {
        if (!scheduleStartPending)
            return false;
        scheduleStartPending = false;
        return true;
    }

    bool scheduleStopWasPending()
    {
        if (!scheduleStopPending)
            return false;
        scheduleStopPending = false;
        return true;
    }

    void updateSchedule(bool enabled, const String &onTime, const String &offTime)
    {
        AppStorage::setScheduleConfig(enabled, onTime, offTime);
        Logger::info(TAG, "Schedule updated from cloud: " + String(enabled ? "ENABLED" : "DISABLED") +
                           " (" + onTime + " - " + offTime + ")");
    }

    void updateAutoResume(bool enabled, uint8_t delayMinutes)
    {
        AppStorage::setAutoResumeConfig(enabled, delayMinutes);
        Logger::info(TAG, "Auto-resume updated from cloud: " + String(enabled ? "ENABLED" : "DISABLED") +
                           " (" + String(delayMinutes) + "m)");
    }

    void triggerAutoResume(uint8_t delayMinutes)
    {
        if (motor.isRunning())
        {
            Logger::info(TAG, "triggerAutoResume: ignored, motor is already running");
            return;
        }

        Logger::warn(TAG, "Triggering auto-resume delay: " + String(delayMinutes) + " minute(s)");
        autoResumePending = true;
        autoResumeDueMs = millis() + (delayMinutes * 60000UL);
    }
}
