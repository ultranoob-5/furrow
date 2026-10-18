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

    // Local time, or false while the clock hasn't been set since power-up.
    bool localNow(struct tm &timeinfo)
    {
        time_t now = time(nullptr);
        if (now < 1704067200) // Valid time after 2024-01-01 (clock synced)
            return false;
        return localtime_r(&now, &timeinfo) != nullptr;
    }

    // True when a schedule is enabled, the clock is set, and the time is
    // outside its ON window - the pump is meant to be off right now.
    bool outsideScheduleWindow()
    {
        if (!AppStorage::scheduleEnabled())
            return false;

        String onTime = AppStorage::scheduleOnTime();
        String offTime = AppStorage::scheduleOffTime();
        if (onTime == offTime || onTime.length() != 5 || offTime.length() != 5)
            return false;

        struct tm timeinfo;
        if (!localNow(timeinfo))
            return false; // time unknown (no internet yet) - auto-resume still works offline

        char buf[6];
        snprintf(buf, sizeof(buf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
        String now = buf;

        // "HH:MM" compares correctly as text; a window crossing midnight
        // (e.g. 22:00-02:00) wraps around.
        bool inWindow = onTime < offTime ? (now >= onTime && now < offTime)
                                         : (now >= onTime || now < offTime);
        return !inWindow;
    }
}

namespace Automation
{
    void begin()
    {
        bool enabled = AppStorage::autoResumeEnabled();
        String lastState = AppStorage::lastMotorState();
        uint8_t delayMin = AppStorage::autoResumeDelayMinutes();

        Logger::info(TAG, "Ready - AutoResume: " + String(enabled ? "ENABLED (" + String(delayMin) + "m)" : "DISABLED") +
                           ", LastMotorState: \"" + lastState + "\"");

        // If auto-resume is enabled and the motor was RUNNING when power was lost,
        // schedule a local start after the configured delay.
        if (enabled && lastState == "RUNNING")
        {
            Logger::warn(TAG, "Auto-resume: motor was RUNNING before power loss. Scheduling start in " +
                               String(delayMin) + " minute(s)");
            autoResumePending = true;
            autoResumeDueMs = millis() + (delayMin * 60000UL);
            // NVS keeps "RUNNING" until the start is attempted, so if power
            // drops again during the countdown the next boot re-arms it.
        }
        else
        {
            if (!enabled && lastState == "RUNNING")
            {
                Logger::info(TAG, "Auto-resume is DISABLED in NVS - skipping local start despite pre-outage RUNNING state");
            }
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
                // One attempt per outage: a confirmed start records
                // "RUNNING" again; a start that doesn't take (tripped
                // overload, no supply) won't be retried on the next power-up.
                AppStorage::setLastMotorState("OFF");

                if (outsideScheduleWindow())
                {
                    // Power came back outside the schedule's ON window (e.g.
                    // after its OFF time) - resuming would run the pump
                    // until the next day's OFF time.
                    Logger::warn(TAG, "Auto-resume skipped - outside the schedule's ON window");
                }
                else
                {
                    autoResumeStartPending = true;
                    Logger::warn(TAG, "Auto-resume delay elapsed - starting motor locally");
                    motor.start();
                }
            }
        }

        // 2. Check Irrigation Schedule (runs at 1Hz cadence)
        if (!AppStorage::scheduleEnabled())
            return;

        if (millis() - lastScheduleCheckMs < 1000)
            return;
        lastScheduleCheckMs = millis();

        struct tm timeinfo;
        if (!localNow(timeinfo))
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
            Logger::warn(TAG, "Schedule: turning OFF at " + String(nowTime));
            // Press Stop even if the CT reads OFF - a wrong reading must
            // never skip a stop (same rule as Motor::stop()). Only expect a
            // tagged OFF transition if it's actually running.
            scheduleStopPending = motor.isRunning();
            scheduleStartPending = false;
            motor.stop();
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
            // Forget the outage too, or the next power cut would resume it.
            if (!motor.isRunning())
                AppStorage::setLastMotorState("OFF");
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
        // A changed OFF time must be able to fire today even if the old one
        // already did, or a running pump isn't stopped until tomorrow. (A
        // changed ON time still waits for tomorrow if today's start already
        // fired, so an edit never adds a surprise start.)
        if (offTime != AppStorage::scheduleOffTime())
            lastOffFiredDate = "";

        AppStorage::setScheduleConfig(enabled, onTime, offTime);
        Logger::info(TAG, "Schedule updated from cloud: " + String(enabled ? "ENABLED" : "DISABLED") +
                           " (" + onTime + " - " + offTime + ")");
    }

    void updateAutoResume(bool enabled, uint8_t delayMinutes)
    {
        AppStorage::setAutoResumeConfig(enabled, delayMinutes);
        Logger::info(TAG, "Auto-resume updated from cloud: " + String(enabled ? "ENABLED" : "DISABLED") +
                           " (" + String(delayMinutes) + "m)");

        // Turning auto-resume off also stops a countdown already running.
        if (!enabled)
            cancelAutoResume();
    }
}
