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

    bool streamStarted = false;

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

        // Only the command stream task carries remote commands.
        if (result.uid() != "commandStream")
            return;

        RealtimeDatabaseResult &rtdb = result.to<RealtimeDatabaseResult>();

        if (!rtdb.isStream())
            return;

        String value = rtdb.to<String>();

        if (value == "start" || value == "stop" || value == "restart" || value == "shutdown" || value == "update")
            pendingCommand = value;
    }

    void handlePendingCommand()
    {
        if (pendingCommand.length() == 0)
            return;

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
            motor.start();
        }
        else if (pendingCommand == "stop")
        {
            Logger::info(TAG, "Remote command: STOP");
            motor.stop();
        }

        pendingCommand = "";

        cloud.publishMotor();

        // Acknowledge / clear the command so it isn't re-applied on the
        // next stream reconnect.
        database.set<String>(aClientMain, commandPath, "none", processData, "clearCommand");
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

    String json = "{";
    json += "\"id\":\"" + device.id() + "\",";
    json += "\"name\":\"" + device.name() + "\",";
    json += "\"owner\":\"" + AppStorage::ownerEmail() + "\",";
    json += "\"firmware\":\"" + device.firmware() + "\",";
    json += "\"ip\":\"" + device.ip() + "\",";
    json += "\"rssi\":" + String(device.rssi()) + ",";
    json += "\"online\":" + String(device.online() ? "true" : "false") + ",";
    json += "\"uptime\":" + String(device.uptime()) + ",";
    // {".sv":"timestamp"} is a Firebase server-value placeholder - the
    // server fills in its own current time on write, not whatever the
    // ESP32 thinks the time is (it has no RTC/NTP). This is what makes
    // "last seen" a real, authoritative fact in the database itself,
    // rather than something only true if some browser happened to be
    // open and watching at the right moment to observe it.
    json += "\"lastSeen\":{\".sv\":\"timestamp\"}";
    json += "}";

    database.set<object_t>(aClientMain, statusPath, object_t(json), processData, "publishDevice");
}

void Cloud::publishMotor()
{
    if (!app.ready())
        return;

    String json = "{";
    json += "\"state\":\"" + String(motor.isRunning() ? "RUNNING" : "OFF") + "\",";
    json += "\"updatedAt\":" + String(millis());
    json += "}";

    database.set<object_t>(aClientMain, motorPath, object_t(json), processData, "publishMotor");
}
