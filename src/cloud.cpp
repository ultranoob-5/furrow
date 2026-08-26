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

        if (value == "start" || value == "stop" || value == "restart" || value == "shutdown" || value == "update" || value == "factory_reset")
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
            motor.start();
            commandConfirmPending = "start";
            commandConfirmSince = millis();
        }
        else if (pendingCommand == "stop")
        {
            Logger::info(TAG, "Remote command: STOP");
            motor.stop();
            commandConfirmPending = "stop";
            commandConfirmSince = millis();
        }

        pendingCommand = "";

        // Gated the same way as the heartbeat below and main.cpp's
        // first-ever publish - never publish a motor-state guess
        // before a real CT reading exists. A remote command arriving
        // in the first few seconds after boot (plausible, if narrow -
        // WiFi connect + Firebase auth + stream setup all take some
        // time too) could otherwise still hit this same "OFF" default
        // even after the setup()/heartbeat paths were fixed for it.
        if (motor.hasReading())
            cloud.publishMotor();

        // Acknowledge / clear the command so it isn't re-applied on the
        // next stream reconnect.
        database.set<String>(aClientMain, commandPath, "none", processData, "clearCommand");
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
            return;
        }

        if (millis() - commandConfirmSince < COMMAND_CONFIRM_TIMEOUT_MS)
            return; // still within the grace window - Star-Delta transitions can take several seconds

        Logger::error(TAG, "Motor failed to " + commandConfirmPending + " - no confirmation within " +
                            String(COMMAND_CONFIRM_TIMEOUT_MS / 1000) + "s");

        Notify::sendWhatsApp("\u26a0\ufe0f " + device.name() + " failed to " + commandConfirmPending +
                              (expectedRunning
                                   ? " - no current detected after 30s. Check the panel."
                                   : " - motor still drawing current after 30s. Check the panel."));

        // Small, rare-path write (only ever happens on an actual
        // failure, never routinely) - a String path concatenation here
        // is fine, unlike the hot 10s-forever publishDevice()/
        // publishMotor() paths. Triggers a new onMotorCommandFailed
        // Cloud Function (RTDB-watched) for the push-notification side;
        // WhatsApp already went out directly above.
        char json[96];
        snprintf(json, sizeof(json), "{\"action\":\"%s\",\"at\":{\".sv\":\"timestamp\"}}", commandConfirmPending.c_str());
        database.set<object_t>(aClientMain, motorPath + "/commandFailure", object_t(json), processData, "commandFailure");

        commandConfirmPending = "";
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
        "\"lastSeen\":{\".sv\":\"timestamp\"}%s,"
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
        waPhoneField);

    if (len < 0 || len >= (int)sizeof(json))
        Logger::error(TAG, "publishDevice: JSON truncated - name/owner/phone unusually long? Buffer is " + String(sizeof(json)) + " bytes");

    database.set<object_t>(aClientMain, statusPath, object_t(json), processData, "publishDevice");
}

void Cloud::publishMotor(const char *startedVia)
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
    // startedVia is only ever non-null from the one call site that
    // just caught a transition into RUNNING (see cloud.h's comment on
    // the declaration) - database.set()'s full-replace semantics mean
    // any call that omits it (every heartbeat, every OFF transition)
    // naturally clears out whatever was there before, so a stale tag
    // never lingers into a later, unrelated publish.
    char json[128];
    int len = (startedVia != nullptr)
        ? snprintf(json, sizeof(json),
              "{\"state\":\"%s\",\"updatedAt\":%lu,\"startedVia\":\"%s\"}",
              motor.isRunning() ? "RUNNING" : "OFF",
              millis(),
              startedVia)
        : snprintf(json, sizeof(json),
              "{\"state\":\"%s\",\"updatedAt\":%lu}",
              motor.isRunning() ? "RUNNING" : "OFF",
              millis());

    if (len < 0 || len >= (int)sizeof(json))
        Logger::error(TAG, "publishMotor: JSON truncated - buffer too small (shouldn't be reachable, all fields are fixed-format)");

    database.set<object_t>(aClientMain, motorPath, object_t(json), processData, "publishMotor");
}

bool Cloud::remoteStartWasPending()
{
    return commandConfirmPending == "start";
}
