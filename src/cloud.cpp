#include "cloud.h"

#define ENABLE_USER_AUTH
#define ENABLE_DATABASE

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <FirebaseClient.h>
#include <esp_sleep.h>
#include <HTTPClient.h>
#include <Update.h>

#include "config.h"
#include "device.h"
#include "motor.h"
#include "network.h"
#include "logger.h"
#include "storage.h"
#include "notify.h"
#include "json_util.h"

Cloud cloud;

namespace
{
    constexpr const char *TAG = "Cloud";

    // ---- Firebase objects -------------------------------------------------

    // Real credential, not NoAuth: this lets Firebase rules distinguish
    // "this firmware" from "any random unauthenticated request" - NoAuth
    // made every write indistinguishable from a stranger's, so no rule
    // could actually restrict control access to specific owners.
    UserAuth deviceAuth(Config::API_KEY, Config::DEVICE_AUTH_EMAIL, Config::DEVICE_AUTH_PASSWORD);

    FirebaseApp app;

    // Separate SSL/async clients for regular requests vs. the long-lived
    // stream connection, as recommended by the FirebaseClient library.
    WiFiClientSecure sslClientMain;
    WiFiClientSecure sslClientStream;

    using AsyncClient = AsyncClientClass;
    AsyncClient aClientMain(sslClientMain);
    AsyncClient aClientStream(sslClientStream);

    RealtimeDatabase database;

    // ---- RTDB paths, resolved once we know the device id ------------------

    String statusPath;
    String motorPath;
    String commandPath;
    String firmwareUrlPath;
    String otaStatusPath;
    String autoResumePath;
    String stateBeforeOutagePath;
    String schedulePath;

    bool streamStarted = false;

    // Auto-resume state: handled locally on ESP32 without requiring Cloud Function polling
    bool autoResumeChecked = false;
    bool autoResumePending = false;
    bool autoResumeStartPending = false;
    unsigned long autoResumeDueMs = 0;
    uint8_t autoResumeDelayMinutes = 5;
    bool autoResumeEnabled = false;
    bool autoResumeDataReceived = false;
    bool stateBeforeOutageReceived = false;
    String stateBeforeOutage = "";

    // Irrigation schedule state: handled autonomously on-device via NTP in IST (UTC+5:30)
    bool scheduleChecked = false;
    bool scheduleEnabled = false;
    String scheduleOnTime = "06:00";
    String scheduleOffTime = "08:00";
    String lastOnFiredDate = "";
    String lastOffFiredDate = "";
    bool scheduleStartPending = false;
    bool scheduleStopPending = false;
    unsigned long lastScheduleCheckMs = 0;

    unsigned long lastHeartbeat = 0;
    constexpr unsigned long HEARTBEAT_INTERVAL_MS = 10000;

    // If Firebase auth hasn't become ready within this long (e.g. the
    // first attempt was lost during a connectivity blip), retry it.
    unsigned long lastAuthAttempt = 0;
    constexpr unsigned long AUTH_RETRY_INTERVAL_MS = 15000;

    // Command received from the stream callback is only recorded here.
    // The actual motor action and any follow-up database writes happen
    // from Cloud::loop(), never from inside the async callback.
    String pendingCommand = "";

    // Set when an "update" command's firmware URL has been fetched and
    // is ready to download+flash - handled from loop(), not from inside
    // the async callback, same reasoning as pendingCommand above (and
    // doubly true here since OTA is a long blocking operation).
    String otaUrlPending = "";
    bool otaRequested = false;
    int otaLastReportedPercent = -1;
    bool otaStatusReset = false;

    // Confirms a remote start/stop command actually took effect, using
    // the same real CT current feedback everything else here trusts -
    // not just "we sent the relay pulse". Deliberately remote-command-
    // only: a physical button press at the panel never routes through
    // this firmware at all (the button is wired straight to the
    // starter's own control loop, in parallel with the relay - see
    // README.md's Wiring section), so there's no "expected outcome" to
    // compare against for that case, only for a command this device
    // itself issued and can therefore verify.
    String commandConfirmPending = ""; // "start" or "stop", empty when nothing's pending
    unsigned long commandConfirmSince = 0;
    constexpr unsigned long COMMAND_CONFIRM_TIMEOUT_MS = 30000;

    void processData(AsyncResult &result);

    void checkTriggerAutoResume()
    {
        if (!autoResumeDataReceived || !stateBeforeOutageReceived)
            return;

        if (autoResumeEnabled && stateBeforeOutage == "RUNNING")
        {
            Logger::warn(TAG, "Auto-resume: pump was RUNNING before outage. Scheduling start in " +
                                String(autoResumeDelayMinutes) + " minute(s)");

            autoResumePending = true;
            autoResumeDueMs = millis() + (autoResumeDelayMinutes * 60000UL);
        }
        else
        {
            if (stateBeforeOutage.length() > 0 && stateBeforeOutage != "null")
            {
                stateBeforeOutage = "";
                database.set<String>(aClientMain, stateBeforeOutagePath, "", processData, "clearStateBeforeOutage");
            }
        }
    }

    void processData(AsyncResult &result)
    {
        if (result.isError())
        {
            Logger::error(TAG, "(" + result.uid() + "): " + result.error().message());

            if (result.uid() == "commandStream")
            {
                // The command stream broke - force Cloud::loop() to
                // start a fresh one next tick rather than leaving the
                // device permanently deaf to remote commands.
                streamStarted = false;
            }
            else if (result.uid() == "fetchFirmwareUrl")
            {
                Logger::error(TAG, "OTA: failed to fetch firmware URL");
            }

            return;
        }

        if (!result.available())
            return;

        if (result.uid() == "fetchFirmwareUrl")
        {
            RealtimeDatabaseResult &rtdb = result.to<RealtimeDatabaseResult>();
            otaUrlPending = rtdb.to<String>();
            otaRequested = true;
            return;
        }

        if (result.uid() == "fetchAutoResume")
        {
            RealtimeDatabaseResult &rtdb = result.to<RealtimeDatabaseResult>();
            String json = rtdb.to<String>();
            autoResumeEnabled = (json.indexOf("\"enabled\":true") >= 0);
            int idx = json.indexOf("\"delayMinutes\":");
            if (idx >= 0)
            {
                int delay = json.substring(idx + 15).toInt();
                if (delay >= 1 && delay <= 10)
                    autoResumeDelayMinutes = delay;
                else
                    autoResumeDelayMinutes = 5;
            }
            autoResumeDataReceived = true;
            checkTriggerAutoResume();
            return;
        }

        if (result.uid() == "fetchStateBeforeOutage")
        {
            RealtimeDatabaseResult &rtdb = result.to<RealtimeDatabaseResult>();
            stateBeforeOutage = rtdb.to<String>();
            stateBeforeOutage.replace("\"", "");
            stateBeforeOutage.trim();
            stateBeforeOutageReceived = true;
            checkTriggerAutoResume();
            return;
        }

        if (result.uid() == "fetchSchedule")
        {
            RealtimeDatabaseResult &rtdb = result.to<RealtimeDatabaseResult>();
            String json = rtdb.to<String>();
            scheduleEnabled = (json.indexOf("\"enabled\":true") >= 0);

            int onIdx = json.indexOf("\"onTime\":\"");
            if (onIdx >= 0)
            {
                String onT = json.substring(onIdx + 10, onIdx + 15);
                if (onT.length() == 5 && onT.charAt(2) == ':')
                    scheduleOnTime = onT;
            }

            int offIdx = json.indexOf("\"offTime\":\"");
            if (offIdx >= 0)
            {
                String offT = json.substring(offIdx + 11, offIdx + 16);
                if (offT.length() == 5 && offT.charAt(2) == ':')
                    scheduleOffTime = offT;
            }

            int lastOnIdx = json.indexOf("\"lastOnFiredDate\":\"");
            if (lastOnIdx >= 0)
            {
                String lastOn = json.substring(lastOnIdx + 19, lastOnIdx + 29);
                if (lastOn.length() == 10 && lastOn.charAt(4) == '-' && lastOn.charAt(7) == '-')
                    lastOnFiredDate = lastOn;
            }

            int lastOffIdx = json.indexOf("\"lastOffFiredDate\":\"");
            if (lastOffIdx >= 0)
            {
                String lastOff = json.substring(lastOffIdx + 20, lastOffIdx + 30);
                if (lastOff.length() == 10 && lastOff.charAt(4) == '-' && lastOff.charAt(7) == '-')
                    lastOffFiredDate = lastOff;
            }

            Logger::info(TAG, "Schedule updated: " + String(scheduleEnabled ? "ENABLED" : "DISABLED") +
                               " (on: " + scheduleOnTime + ", off: " + scheduleOffTime + ")");
            return;
        }

        // Only the command stream task carries remote commands.
        if (result.uid() != "commandStream")
            return;

        RealtimeDatabaseResult &rtdb = result.to<RealtimeDatabaseResult>();

        if (!rtdb.isStream())
            return;

        String value = rtdb.to<String>();

        if (value == "start" || value == "stop" || value == "restart" || value == "shutdown" ||
            value == "update" || value == "factory_reset" || value == "cancel_auto_resume" ||
            value == "sync_schedule")
            pendingCommand = value;
    }

    void handlePendingCommand()
    {
        if (pendingCommand.length() == 0)
            return;

        if (pendingCommand == "cancel_auto_resume")
        {
            Logger::warn(TAG, "Remote command: CANCEL AUTO-RESUME");
            autoResumePending = false;
            pendingCommand = "";
            database.set<String>(aClientMain, commandPath, "none", processData, "clearCommand");
            return;
        }

        if (pendingCommand == "sync_schedule")
        {
            Logger::info(TAG, "Remote command: SYNC_SCHEDULE");
            pendingCommand = "";
            database.set<String>(aClientMain, commandPath, "none", processData, "clearCommand");
            database.get(aClientMain, schedulePath, processData, false, "fetchSchedule");
            return;
        }

        if (pendingCommand == "restart")
        {
            Logger::warn(TAG, "Remote command: RESTART");

            // Clear the command before rebooting - otherwise, once the
            // stream reconnects after boot, it'll see the same
            // "restart" value still sitting there and reboot forever.
            database.set<String>(aClientMain, commandPath, "none", processData, "clearCommand");

            delay(1000); // let the clear request actually go out before we reboot
            ESP.restart();
        }

        if (pendingCommand == "shutdown")
        {
            Logger::warn(TAG, "Remote command: SHUTDOWN - deep sleep, no wake source configured");

            // Same reasoning as restart: clear the command first, so it
            // doesn't immediately shut back down the moment someone
            // physically powers it back on and it reconnects.
            database.set<String>(aClientMain, commandPath, "none", processData, "clearCommand");

            delay(1000);

            // No wake source configured on purpose: this is a genuine
            // "off" state. Recovery requires physically power-cycling
            // the board or pressing its EN/reset button - there is no
            // remote way to turn it back on from here.
            esp_deep_sleep_start();
        }

        if (pendingCommand == "factory_reset")
        {
            Logger::warn(TAG, "Remote command: FACTORY RESET - clearing all stored config, restarting into setup mode");

            AppStorage::factoryReset();

            // Same reasoning as restart/shutdown: clear the command
            // first. Unlike shutdown, this device WILL come back up
            // and start broadcasting its own Furrow-Setup-XXXX WiFi
            // network again immediately after rebooting - it's not
            // gone, just unreachable via Firebase until someone
            // physically re-provisions it through the captive portal.
            database.set<String>(aClientMain, commandPath, "none", processData, "clearCommand");

            delay(1000);
            ESP.restart();
        }

        if (pendingCommand == "update")
        {
            Logger::warn(TAG, "Remote command: UPDATE - fetching firmware URL");

            database.get(aClientMain, firmwareUrlPath, processData, false, "fetchFirmwareUrl");

            pendingCommand = "";
            database.set<String>(aClientMain, commandPath, "none", processData, "clearCommand");
            return;
        }

        if (pendingCommand == "start")
        {
            Logger::info(TAG, "Remote command: START");
            scheduleStartPending = false;
            scheduleStopPending = false;
            commandConfirmPending = "start";
            commandConfirmSince = millis();
            motor.start();
        }
        else if (pendingCommand == "stop")
        {
            Logger::info(TAG, "Remote command: STOP");
            autoResumePending = false; // Stop cancels any active auto-resume timer
            scheduleStartPending = false;
            scheduleStopPending = false;
            commandConfirmPending = "stop";
            commandConfirmSince = millis();
            motor.stop();
        }

        pendingCommand = "";

        // Gated the same way as the heartbeat below and main.cpp's
        // first-ever publish - never publish a motor-state guess
        // before a real CT reading exists. A remote command arriving
        // in the first few seconds after boot (plausible, if narrow -
        // WiFi connect + Firebase auth + stream setup all take some
        // time too) could otherwise still hit this same "OFF" default
        // even after the setup()/heartbeat paths were fixed for it.
        // In devMode, main.cpp handles the state change publish directly.
        if (motor.hasReading() && !motor.isDevelopment())
            cloud.publishMotor();

        // Acknowledge / clear the command so it isn't re-applied on the
        // next stream reconnect.
        database.set<String>(aClientMain, commandPath, "none", processData, "clearCommand");
    }

    void checkAutoResume()
    {
        if (!autoResumePending)
            return;

        // If the motor is already running, cancel auto-resume
        if (motor.isRunning())
        {
            Logger::info(TAG, "Auto-resume cancelled - motor already running");
            autoResumePending = false;
            return;
        }

        if ((long)(millis() - autoResumeDueMs) >= 0)
        {
            autoResumePending = false;
            autoResumeStartPending = true;
            if (stateBeforeOutage.length() > 0)
            {
                stateBeforeOutage = "";
                database.set<String>(aClientMain, stateBeforeOutagePath, "", processData, "clearStateBeforeOutage");
            }
            Logger::warn(TAG, "Auto-resume delay elapsed - starting motor");
            motor.start();
        }
    }

    void checkSchedule()
    {
        if (!scheduleEnabled)
            return;

        if (!Network::isConnected() || !app.ready())
            return;

        if (millis() - lastScheduleCheckMs < 1000)
            return;
        lastScheduleCheckMs = millis();

        time_t now = time(nullptr);
        if (now < 1704067200) // Valid time after 2024-01-01
            return;

        struct tm timeinfo;
        if (!localtime_r(&now, &timeinfo))
            return;

        char nowTime[6];
        snprintf(nowTime, sizeof(nowTime), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);

        char nowDate[11];
        snprintf(nowDate, sizeof(nowDate), "%04d-%02d-%02d",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);

        // Guards against a misconfigured schedule (on time == off time)
        if (scheduleOnTime == scheduleOffTime)
            return;

        bool onDue = (scheduleOnTime == nowTime && lastOnFiredDate != nowDate);
        bool offDue = (scheduleOffTime == nowTime && lastOffFiredDate != nowDate);

        if (!onDue && !offDue)
            return;

        // Scheduled turn on and off must only execute when the device has active internet
        if (!Network::hasInternet())
        {
            Logger::warn(TAG, "Schedule: trigger due at " + String(nowTime) + 
                              " but device has no active internet - skipping");
            return;
        }

        // Start schedule trigger (fires once per calendar date)
        if (onDue)
        {
            lastOnFiredDate = nowDate;
            database.set<String>(aClientMain, schedulePath + "/lastOnFiredDate", String(nowDate), processData, "recordScheduleFired");

            if (!motor.isRunning())
            {
                Logger::warn(TAG, "Schedule: turning ON at " + String(nowTime));
                scheduleStartPending = true;
                scheduleStopPending = false;
                if (!motor.isDevelopment())
                {
                    commandConfirmPending = "start";
                    commandConfirmSince = millis();
                }
                motor.start();
            }
            else
            {
                Logger::info(TAG, "Schedule: start time reached but motor already RUNNING");
            }
        }

        // Stop schedule trigger (fires once per calendar date)
        if (offDue)
        {
            lastOffFiredDate = nowDate;
            database.set<String>(aClientMain, schedulePath + "/lastOffFiredDate", String(nowDate), processData, "recordScheduleFired");

            if (motor.isRunning())
            {
                Logger::warn(TAG, "Schedule: turning OFF at " + String(nowTime));
                scheduleStopPending = true;
                scheduleStartPending = false;
                if (!motor.isDevelopment())
                {
                    commandConfirmPending = "stop";
                    commandConfirmSince = millis();
                }
                motor.stop();
            }
            else
            {
                Logger::info(TAG, "Schedule: stop time reached and motor already OFF");
            }
        }
    }

    // Checked every Cloud::loop() iteration - cheap no-op when nothing's
    // pending. If a start/stop command was just dispatched above,
    // compares its expected outcome against motor.isRunning() (the real
    // CT-sensed state, same source of truth as everything else) rather
    // than trusting the relay pulse alone succeeded. A new start/stop
    // command overwrites commandConfirmPending before this ever sees
    // the old one time out, so issuing a second command before the
    // first's window elapses can never produce a false failure alert
    // for the abandoned one - only the latest command's outcome is
    // ever actually checked.
    void checkCommandConfirmation()
    {
        if (commandConfirmPending.length() == 0)
            return;

        bool expectedRunning = (commandConfirmPending == "start");

        if (motor.isRunning() == expectedRunning)
        {
            // Confirmed - the command took effect. Nothing to publish;
            // publishMotor() already covers the real state change.
            commandConfirmPending = "";
            scheduleStopPending = false;
            return;
        }

        if (millis() - commandConfirmSince < COMMAND_CONFIRM_TIMEOUT_MS)
            return; // still within the grace window - Star-Delta transitions can take several seconds

        if (motor.isDevelopment())
        {
            commandConfirmPending = "";
            scheduleStopPending = false;
            return;
        }

        Logger::error(TAG, "Motor failed to " + commandConfirmPending + " - no confirmation within " +
                            String(COMMAND_CONFIRM_TIMEOUT_MS / 1000) + "s");

        // Small, rare-path write (only ever happens on an actual
        // failure, never routinely) - triggers onMotorCommandFailed
        // Cloud Function (RTDB-watched) for both Push and WhatsApp
        // notifications in elder-friendly wording.
        char json[96];
        snprintf(json, sizeof(json), "{\"action\":\"%s\",\"at\":{\".sv\":\"timestamp\"}}", commandConfirmPending.c_str());
        database.set<object_t>(aClientMain, motorPath + "/commandFailure", object_t(json), processData, "commandFailure");

        commandConfirmPending = "";
        scheduleStartPending = false;
        scheduleStopPending = false;
        autoResumeStartPending = false;
    }

    // Downloads and flashes new firmware from a URL, then restarts.
    // Blocking by design - there's nothing useful to do concurrently
    // with a firmware flash, and the device is about to reboot either
    // way. Manually triggered only (via the "update" command); does not
    // auto-check or auto-apply anything on its own.
    // performOTA() below is a long blocking call, not running inside
    // the normal loop()/app.loop() cadence - a plain database.set()
    // would just sit queued until we returned. This forces a few
    // pumps of the async client right now so progress updates actually
    // reach Firebase live, not all at once after the fact.
    // phase/message are never escaped here, unlike name/owner above -
    // every call site passes a fixed literal string (see the
    // performOTA()/handlePendingCommand() calls below), never anything
    // a person typed or a remote server sent, so there's nothing to
    // guard against.
    void publishOtaStatus(const String &phase, int percent, const String &message = "")
    {
        String json = "{";
        json += "\"phase\":\"" + phase + "\",";
        json += "\"percent\":" + String(percent);
        if (message.length() > 0)
            json += ",\"message\":\"" + message + "\"";
        json += "}";

        database.set<object_t>(aClientMain, otaStatusPath, object_t(json), processData, "otaStatus");

        for (int i = 0; i < 5; i++)
        {
            app.loop();
            database.loop();
            delay(10);
        }
    }

    void performOTA(const String &url)
    {
        if (url.length() == 0)
        {
            Logger::error(TAG, "OTA: no firmware URL set - aborting");
            publishOtaStatus("failed", 0, "No firmware URL was set");
            Notify::sendWhatsApp("\u26a0\ufe0f " + device.name() + " update failed: no firmware URL was set");
            return;
        }

        Logger::warn(TAG, "OTA: downloading firmware from " + url);
        publishOtaStatus("downloading", 0);

        WiFiClientSecure otaClient;
        otaClient.setInsecure(); // same tradeoff as the rest of this project's HTTPS use

        HTTPClient http;
        http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS); // GitHub release assets redirect to a CDN
        http.begin(otaClient, url);

        int httpCode = http.GET();

        if (httpCode != HTTP_CODE_OK)
        {
            Logger::error(TAG, "OTA: download failed, HTTP " + String(httpCode));
            publishOtaStatus("failed", 0, "Download failed (HTTP " + String(httpCode) + ")");
            Notify::sendWhatsApp("\u26a0\ufe0f " + device.name() + " update failed: download error (HTTP " + String(httpCode) + ")");
            http.end();
            return;
        }

        int contentLength = http.getSize();

        if (contentLength <= 0)
        {
            Logger::error(TAG, "OTA: server didn't report a valid file size");
            publishOtaStatus("failed", 0, "Server didn't report a valid file size");
            Notify::sendWhatsApp("\u26a0\ufe0f " + device.name() + " update failed: server didn't report a valid file size");
            http.end();
            return;
        }

        if (!Update.begin(contentLength))
        {
            Logger::error(TAG, "OTA: not enough free space for a " + String(contentLength) + " byte update");
            publishOtaStatus("failed", 0, "Not enough free space on device");
            Notify::sendWhatsApp("\u26a0\ufe0f " + device.name() + " update failed: not enough free space on device");
            http.end();
            return;
        }

        Logger::info(TAG, "OTA: writing " + String(contentLength) + " bytes...");

        // Download and flash happen interleaved inside writeStream() -
        // read a chunk, write it, repeat - not two separate passes, so
        // one combined percentage covers both rather than faking a
        // download-then-flash split that doesn't reflect what's really
        // happening.
        otaLastReportedPercent = -1;

        Update.onProgress([](size_t written, size_t total) {
            if (total == 0)
                return;

            int percent = (int)((written * 100) / total);

            // Throttled to every 5% (and always the final 100%) - this
            // callback fires very frequently, and neither Serial nor
            // Firebase needs every single intermediate byte count.
            if (percent == otaLastReportedPercent)
                return;
            if (percent % 5 != 0 && percent != 100)
                return;

            otaLastReportedPercent = percent;

            Logger::info(TAG, "OTA: downloading & flashing... " + String(percent) + "%");
            publishOtaStatus("downloading", percent);
        });

        WiFiClient *stream = http.getStreamPtr();
        size_t written = Update.writeStream(*stream);

        http.end();

        if (written != (size_t)contentLength)
        {
            Logger::error(TAG, "OTA: only wrote " + String(written) + " of " + String(contentLength) + " bytes - aborting");
            publishOtaStatus("failed", otaLastReportedPercent, "Incomplete download/write");
            Notify::sendWhatsApp("\u26a0\ufe0f " + device.name() + " update failed: incomplete download/write");
            Update.abort();
            return;
        }

        if (!Update.end() || !Update.isFinished())
        {
            Logger::error(TAG, "OTA: update did not complete cleanly (error " + String(Update.getError()) + ")");
            publishOtaStatus("failed", 100, "Update did not finalize cleanly");
            Notify::sendWhatsApp("\u26a0\ufe0f " + device.name() + " update failed: did not finalize cleanly");
            return;
        }

        Logger::info(TAG, "OTA: success - restarting into new firmware");
        publishOtaStatus("restarting", 100);
        Notify::sendWhatsApp("\u2705 " + device.name() + " firmware updated successfully - restarting");

        delay(1000);
        ESP.restart();
    }
}

void Cloud::begin()
{
    statusPath  = "/devices/" + device.id() + "/status";
    motorPath   = "/devices/" + device.id() + "/motor";
    commandPath = "/devices/" + device.id() + "/command/action";
    firmwareUrlPath = "/devices/" + device.id() + "/command/firmwareUrl";
    otaStatusPath = "/devices/" + device.id() + "/ota";
    autoResumePath = "/devices/" + device.id() + "/autoResume";
    stateBeforeOutagePath = "/devices/" + device.id() + "/motor/stateBeforeOutage";
    schedulePath = "/devices/" + device.id() + "/schedule";

    // Skip TLS certificate verification for simplicity in v1.0.
    // Consider pinning Google's root CA for production use.
    sslClientMain.setInsecure();
    sslClientStream.setInsecure();

    aClientStream.setSSEFilters("get,put,patch,keep-alive,cancel,auth_revoked");

    Logger::info(TAG, "Ready");

    initializeApp(aClientMain, app, getAuth(deviceAuth), processData, "authTask");
    lastAuthAttempt = millis();

    app.getApp<RealtimeDatabase>(database);
    database.url(Config::DATABASE_URL);
}

void Cloud::loop()
{
    app.loop();
    database.loop();

    if (Network::consumeReconnectEvent())
    {
        // Wi-Fi dropped and came back. The Firebase stream connection is
        // stale, so force it to restart cleanly instead of assuming the
        // library will silently recover it, and publish right away
        // instead of waiting for the next heartbeat tick.
        Logger::info(TAG, "WiFi reconnected - restarting Firebase stream");
        streamStarted = false;
        lastHeartbeat = 0;
        autoResumeChecked = false;
        autoResumeDataReceived = false;
        stateBeforeOutageReceived = false;
        scheduleChecked = false;

        unsigned long downtimeMs = Network::lastDisconnectDurationMs();

        if (downtimeMs >= 30000)
        {
            Notify::sendWhatsApp("\u26a0\ufe0f " + device.name() + " was offline for " +
                                  String(downtimeMs / 1000) + "s - just reconnected");
        }
    }

    if (!app.ready())
    {
        // Auth watchdog: if we've been stuck waiting too long, retry it.
        // Covers the case where the initial auth attempt was lost during
        // a connectivity blip and never came back on its own.
        if (Network::isConnected() && millis() - lastAuthAttempt >= AUTH_RETRY_INTERVAL_MS)
        {
            Logger::warn(TAG, "Auth not ready - retrying");
            lastAuthAttempt = millis();
            initializeApp(aClientMain, app, getAuth(deviceAuth), processData, "authTask");
        }

        return;
    }

    if (!streamStarted)
    {
        database.get(aClientStream, commandPath, processData, true /* SSE mode */, "commandStream");
        streamStarted = true;

        Logger::info(TAG, "Listening for remote commands");
    }

    if (!autoResumeChecked)
    {
        autoResumeChecked = true;
        autoResumeDataReceived = false;
        stateBeforeOutageReceived = false;
        database.get(aClientMain, autoResumePath, processData, false, "fetchAutoResume");
        database.get(aClientMain, stateBeforeOutagePath, processData, false, "fetchStateBeforeOutage");
    }

    if (!scheduleChecked)
    {
        scheduleChecked = true;
        database.get(aClientMain, schedulePath, processData, false, "fetchSchedule");
    }

    if (!otaStatusReset)
    {
        // Clears any leftover OTA status from before this boot (whether
        // this boot followed a successful update or an unrelated
        // restart) - otherwise a stale "downloading 87%" or
        // "restarting" would sit on the dashboard forever.
        database.set<object_t>(aClientMain, otaStatusPath, object_t("{\"phase\":\"idle\",\"percent\":0}"), processData, "otaStatusReset");
        otaStatusReset = true;
    }

    handlePendingCommand();

    checkCommandConfirmation();

    checkAutoResume();

    checkSchedule();

    if (otaRequested)
    {
        otaRequested = false;
        performOTA(otaUrlPending);
        // If performOTA() succeeded it already restarted and this line
        // never runs; if it failed, we fall through and keep operating
        // normally on the current firmware.
    }

    if (millis() - lastHeartbeat >= HEARTBEAT_INTERVAL_MS)
    {
        lastHeartbeat = millis();

        publishDevice();

        // Same guard as main.cpp's first-ever publish and
        // handlePendingCommand()'s republish above - if the very
        // first heartbeat happens to land before the first real CT
        // reading (plausible if the reading is still slow - see
        // current_sensor.h), skip motor state this one time rather
        // than publishing CurrentSensor's not-running default as
        // fact. Once hasReading() is true this is never false again
        // for the rest of this boot.
        if (motor.hasReading())
            publishMotor();
    }
}

bool Cloud::connected()
{
    return device.online() && app.ready();
}

void Cloud::publishDevice()
{
    if (!app.ready())
        return;

    // device.id()/firmware()/ip() are all internally generated (MAC-
    // derived, compile-time, WiFi-library-provided) - safe as-is. name
    // and owner are typed by a person during provisioning, so they go
    // through JsonUtil::escape() to guard the JSON structure itself;
    // see json_util.h for what an unescaped '"' here silently does to
    // this write.
    //
    // Fixed char buffers + snprintf instead of String concatenation -
    // this function runs every HEARTBEAT_INTERVAL_MS (10s) for as long
    // as the device is up, potentially months at a time for unattended
    // farm equipment. The old code built this JSON with ~10 separate
    // String += calls; each one can reallocate the whole buffer on the
    // heap as it grows, so a routine 10s-forever heartbeat was doing
    // up to 10 heap allocate/free cycles every single time - exactly
    // the pattern that fragments a long-uptime heap. One snprintf()
    // into a stack buffer needs zero heap allocations for the JSON
    // itself (the String objects for the escaped name/owner/phone
    // still exist below, since JsonUtil::escape() has to build
    // arbitrary-length output - only the repeated-growth pattern is
    // what's fixed here, not String's mere existence anywhere in this
    // function).
    //
    // 640 bytes is generous on purpose, not tightly measured: name is
    // capped at 40 chars by the provisioning form but escaping can
    // roughly double a pathological all-quotes input, owner email has
    // no hard cap, and getting this wrong silently truncates a real
    // device's status write - cheap insurance on an ESP32's stack.
    String escapedName = JsonUtil::escape(device.name());
    String escapedOwner = JsonUtil::escape(AppStorage::ownerEmail());

    // Published (not just kept in local flash) specifically so the
    // power-loss watchdog (.github/workflows/power-watchdog.yml) can
    // know who to alert - it runs outside this device entirely and
    // has no other way to read AppStorage. Same read exposure as the
    // owner email above (this whole /devices tree is publicly
    // readable per the RTDB rules) - only published if actually set,
    // so a device with no WhatsApp config configured doesn't publish
    // an empty string.
    String waPhone = AppStorage::whatsAppPhone();
    char waPhoneField[150] = "";

    if (waPhone.length() > 0)
    {
        String escapedPhone = JsonUtil::escape(waPhone);
        snprintf(waPhoneField, sizeof(waPhoneField), ",\"whatsappPhone\":\"%s\"", escapedPhone.c_str());
    }

    char devModeField[24] = "";
    if (AppStorage::isDevelopmentDevice())
    {
        snprintf(devModeField, sizeof(devModeField), ",\"isDevelopment\":true");
    }

    char json[640];
    int len = snprintf(json, sizeof(json),
        "{\"id\":\"%s\",\"name\":\"%s\",\"owner\":\"%s\",\"firmware\":\"%s\",\"ip\":\"%s\","
        "\"rssi\":%d,\"online\":%s,\"uptime\":%lu,"
        // {".sv":"timestamp"} is a Firebase server-value placeholder -
        // the server fills in its own current time on write, not
        // whatever the ESP32 thinks the time is (it has no RTC/NTP).
        // This is what makes "last seen" a real, authoritative fact
        // in the database itself, rather than something only true if
        // some browser happened to be open and watching at the right
        // moment to observe it.
        "\"lastSeen\":{\".sv\":\"timestamp\"}%s%s,"
        // Resets the power-watchdog's dedup flag every single
        // heartbeat this device is alive to send one - the watchdog
        // only sets this true when it detects an outage, so as long
        // as the device is reporting normally, this stays false
        // without the device needing to know anything about the
        // watchdog's own state.
        "\"powerAlertSent\":false}",
        device.id().c_str(),
        escapedName.c_str(),
        escapedOwner.c_str(),
        device.firmware().c_str(),
        device.ip().c_str(),
        device.rssi(),
        device.online() ? "true" : "false",
        device.uptime(),
        waPhoneField,
        devModeField);

    if (len < 0 || len >= (int)sizeof(json))
        Logger::error(TAG, "publishDevice: JSON truncated - name/owner/phone unusually long? Buffer is " + String(sizeof(json)) + " bytes");

    database.set<object_t>(aClientMain, statusPath, object_t(json), processData, "publishDevice");
}

void Cloud::publishMotor(const char *startedVia, const char *stoppedVia)
{
    if (!app.ready())
        return;

    // Fixed char buffer + snprintf instead of String concatenation -
    // this function runs every HEARTBEAT_INTERVAL_MS (10s) for as long
    // as the device is up, potentially months at a time for unattended
    // farm equipment. String's += operator reallocates on the heap
    // each time the buffer needs to grow, and doing that on a fixed
    // 10s cadence indefinitely is exactly the kind of repeated
    // allocate/free pattern that fragments a long-uptime heap. A
    // stack-allocated char[] with one single snprintf() call needs
    // zero heap allocations at all. Not applied file-wide - see the
    // sizing comment on publishDevice() below for why the much rarer/
    // one-time paths in this file (path setup, OTA, remote commands)
    // were deliberately left as String.
    //
    // startedVia and stoppedVia are only ever non-null from the call site
    // that just caught a state transition (see cloud.h) - database.set()'s
    // full-replace semantics mean any call that omits it (every routine
    // heartbeat) naturally clears out whatever was there before, so a stale
    // tag never lingers into a later, unrelated publish.
    char json[128];
    int len = 0;
    if (startedVia != nullptr)
    {
        len = snprintf(json, sizeof(json),
                       "{\"state\":\"%s\",\"updatedAt\":%lu,\"startedVia\":\"%s\"}",
                       motor.isRunning() ? "RUNNING" : "OFF",
                       millis(),
                       startedVia);
    }
    else if (stoppedVia != nullptr)
    {
        len = snprintf(json, sizeof(json),
                       "{\"state\":\"%s\",\"updatedAt\":%lu,\"stoppedVia\":\"%s\"}",
                       motor.isRunning() ? "RUNNING" : "OFF",
                       millis(),
                       stoppedVia);
    }
    else
    {
        len = snprintf(json, sizeof(json),
                       "{\"state\":\"%s\",\"updatedAt\":%lu}",
                       motor.isRunning() ? "RUNNING" : "OFF",
                       millis());
    }

    if (len < 0 || len >= (int)sizeof(json))
        Logger::error(TAG, "publishMotor: JSON truncated - buffer too small (shouldn't be reachable, all fields are fixed-format)");

    database.set<object_t>(aClientMain, motorPath, object_t(json), processData, "publishMotor");
}

bool Cloud::remoteStartWasPending()
{
    return commandConfirmPending == "start";
}

bool Cloud::remoteStopWasPending()
{
    return commandConfirmPending == "stop" && !scheduleStopPending;
}

bool Cloud::isAutoResumePending()
{
    return autoResumePending;
}

bool Cloud::autoResumeStartWasPending()
{
    return autoResumeStartPending;
}

void Cloud::cancelAutoResume()
{
    if (autoResumePending)
    {
        Logger::info(TAG, "Auto-resume cancelled");
        autoResumePending = false;
    }
    autoResumeStartPending = false;
    scheduleStartPending = false;
    scheduleStopPending = false;
    if (stateBeforeOutage.length() > 0)
    {
        stateBeforeOutage = "";
        database.set<String>(aClientMain, stateBeforeOutagePath, "", processData, "clearStateBeforeOutage");
    }
}

bool Cloud::scheduleStartWasPending()
{
    return scheduleStartPending;
}

bool Cloud::scheduleStopWasPending()
{
    return scheduleStopPending;
}

