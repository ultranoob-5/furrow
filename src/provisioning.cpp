#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>

#include "provisioning.h"
#include "storage.h"
#include "logger.h"
#include "json_util.h"

namespace
{
    constexpr const char *TAG = "Provisioning";
    constexpr byte DNS_PORT = 53;

    // How long to try the new WiFi details before calling them wrong.
    constexpr unsigned long WIFI_TEST_TIMEOUT_MS = 15000;

    // After a successful test, keep the page up this long so the phone can
    // show the result before the device restarts.
    constexpr unsigned long RESTART_DELAY_MS = 8000;

    DNSServer dnsServer;
    WebServer server(80);

    String cachedScanJson = "[]";

    // What the person typed. After an error the form shows these again, not
    // the stored values, so nothing has to be retyped (except the password).
    struct Form
    {
        String ssid;
        String password;
        String deviceName;
        String owner;
        String waPhone;
        bool devMode = false;
    };
    Form form;

    // The WiFi details are tried before anything is saved, so a wrong
    // password can't leave the device stuck off the network.
    enum class Test
    {
        Idle,
        Testing,
        Ok,
        Failed
    };
    Test testState = Test::Idle;
    unsigned long testStartMs = 0;
    unsigned long restartAtMs = 0;
    uint8_t lastDisconnectReason = 0;
    String failureMessage; // HTML, already escaped

    String apName()
    {
        String mac = WiFi.macAddress(); // "AA:BB:CC:DD:EE:FF"
        mac.replace(":", "");
        String suffix = mac.substring(mac.length() - 4);
        suffix.toUpperCase();
        return "Furrow-Setup-" + suffix;
    }

    // Typed and stored values go into the page - a name containing a quote
    // or < would otherwise break (or inject into) it.
    String htmlEscape(const String &s)
    {
        String out;
        out.reserve(s.length() + 8);
        for (size_t i = 0; i < s.length(); i++)
        {
            char c = s[i];
            switch (c)
            {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
            }
        }
        return out;
    }

    String failureText(uint8_t reason, const String &ssid)
    {
        String name = "<b>" + htmlEscape(ssid) + "</b>";
        switch (reason)
        {
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_AUTH_EXPIRE:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return "Couldn&rsquo;t join " + name + " - the password looks wrong. Please check it and try again.";
        case WIFI_REASON_NO_AP_FOUND:
            return "Couldn&rsquo;t find " + name + ". Check the name, that it&rsquo;s a 2.4 GHz network, and that the router is in range.";
        default:
            return "Couldn&rsquo;t connect to " + name + ". Check the password and that the router is in range, then try again.";
        }
    }

    const char *PAGE_HEAD = R"HTML(<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Furrow Setup</title>
<style>
  body { font-family: -apple-system, sans-serif; background:#14161A; color:#ECE9E2; margin:0; padding:24px; }
  .card { max-width:360px; margin:0 auto; background:#1B1E23; border:1px solid #2E333B; border-radius:12px; padding:24px; }
  h1 { font-size:18px; margin:0 0 4px; }
  p.sub { color:#868C97; font-size:13px; margin:0 0 16px; }
  label { display:block; font-size:12px; color:#868C97; margin:14px 0 6px; text-transform:uppercase; letter-spacing:0.06em; }
  .hint { font-size:11px; color:#868C97; margin-top:4px; }
  select, input { width:100%; box-sizing:border-box; padding:10px 12px; border-radius:8px; border:1px solid #2E333B; background:#21252B; color:#ECE9E2; font-size:14px; }
  button { width:100%; margin-top:20px; padding:12px 0; border:none; border-radius:8px; background:#E8A33D; color:#241804; font-weight:700; font-size:14px; cursor:pointer; }
  button:disabled { opacity:0.5; cursor:default; }
  .msg { font-size:13px; line-height:1.5; border-radius:8px; padding:10px 12px; margin:0 0 6px; }
  .msg-error { background:rgba(225,82,65,0.12); border:1px solid rgba(225,82,65,0.4); color:#F08A7E; }
  .msg-info { background:rgba(232,163,61,0.12); border:1px solid rgba(232,163,61,0.4); color:#ECE9E2; }
  .msg-ok { background:rgba(73,182,117,0.12); border:1px solid rgba(73,182,117,0.4); color:#ECE9E2; }
  details { margin-top:18px; font-size:12px; color:#868C97; }
  summary { cursor:pointer; }
  .check-row { display:flex; align-items:flex-start; gap:10px; margin-top:12px; cursor:pointer; text-transform:none; letter-spacing:normal; }
  .check-row input[type="checkbox"] { width:18px; height:18px; margin-top:2px; flex-shrink:0; accent-color:#E8A33D; cursor:pointer; }
  .check-row-text { font-size:13px; color:#ECE9E2; }
  .check-row-sub { font-size:11px; color:#868C97; margin-top:2px; }
</style>
</head>
<body>
  <div class="card">
    <h1>Furrow</h1>
)HTML";

    const char *PAGE_TAIL = R"HTML(
  </div>
</body>
</html>
)HTML";

    // How to get back to this page later, e.g. after changing the router.
    const char *CHANGE_WIFI_HELP =
        "To change WiFi later (new router or password): switch the device on and, "
        "within 5 seconds, hold its BOOT button for 3 seconds. This setup WiFi comes back.";

    String successPage()
    {
        String html = PAGE_HEAD;
        html += "<div class=\"msg msg-ok\">&#10003; Connected to <b>" + htmlEscape(form.ssid) +
                "</b> and saved. The device is restarting and will show up in your dashboard in a minute.</div>";
        html += "<p class=\"sub\" style=\"margin-top:14px\">";
        html += CHANGE_WIFI_HELP;
        html += "</p>";
        html += PAGE_TAIL;
        return html;
    }

    String formPage(const String &message = "", const char *kind = "")
    {
        String html = PAGE_HEAD;
        html += "<p class=\"sub\">Connect this device to your WiFi network</p>";

        if (message.length() > 0)
            html += "<div class=\"msg msg-" + String(kind) + "\">" + message + "</div>";

        const bool testing = (testState == Test::Testing);

        html += R"HTML(
    <form method="POST" action="/save">
      <label>WiFi Network</label>
      <select name="ssid" id="ssid" data-current=")HTML" + htmlEscape(form.ssid) + R"HTML("><option value="">Scanning...</option></select>
      <div class="hint">Only 2.4 GHz networks work - the ESP32 can&rsquo;t use 5 GHz.</div>
      <label>Or type the network name manually (use this if yours isn't listed above)</label>
      <input type="text" name="ssid_manual" id="ssid_manual" placeholder="Leave blank to use the dropdown">
      <label>Password</label>
      <input type="password" name="password" placeholder="WiFi password">
      <label>Device Name</label>
      <input type="text" name="devicename" value=")HTML" + htmlEscape(form.deviceName) + R"HTML(" placeholder="e.g. Farm Pump" maxlength="40">
      <label>Owner Email</label>
      <input type="email" name="owner" value=")HTML" + htmlEscape(form.owner) + R"HTML(" placeholder="you@example.com" maxlength="60">
      <label>WhatsApp Alerts (optional)</label>
      <input type="text" name="wa_phone" value=")HTML" + htmlEscape(form.waPhone) + R"HTML(" placeholder="Your number, digits only e.g. 919876543210" maxlength="20">
      <details)HTML" + String(form.devMode ? " open" : "") + R"HTML(>
        <summary>Advanced (bench testing only)</summary>
        <label class="check-row">
          <input type="checkbox" name="dev_mode" value="1")HTML" + String(form.devMode ? " checked" : "") + R"HTML(>
          <div>
            <div class="check-row-text">Development Device</div>
            <div class="check-row-sub">Ignores the current sensor - only for testing without a motor. Leave this off on a real pump.</div>
          </div>
        </label>
      </details>
      <button type="submit" id="saveBtn")HTML" + String(testing ? " disabled" : "") + R"HTML(>Save &amp; Connect</button>
    </form>
    <p class="sub" style="margin:16px 0 0">)HTML";
        html += CHANGE_WIFI_HELP;
        html += R"HTML(</p>
  <script>
    fetch('/scan').then(r => r.json()).then(list => {
      const sel = document.getElementById('ssid');
      const manual = document.getElementById('ssid_manual');
      const current = sel.dataset.current || '';
      sel.innerHTML = '';
      if (list.length === 0) {
        const opt = document.createElement('option');
        opt.value = ''; opt.textContent = 'No networks found - type yours below';
        sel.appendChild(opt);
      }
      list.forEach(ssid => {
        const opt = document.createElement('option');
        opt.value = ssid; opt.textContent = ssid;
        if (ssid === current) opt.selected = true;
        sel.appendChild(opt);
      });
      // The network saved or typed last isn't in range of the scan: keep it
      // in the manual box rather than silently switching to another one.
      if (current && !list.includes(current)) {
        manual.value = current;
        sel.disabled = true;
      }
    }).catch(() => {
      const sel = document.getElementById('ssid');
      sel.innerHTML = '<option value="">Scan failed - type yours below</option>';
    });

    // If the manual field is used, it wins over the dropdown - disable
    // the dropdown's value so an accidental leftover selection can't
    // silently override what was actually typed.
    document.getElementById('ssid_manual').addEventListener('input', function(){
      document.getElementById('ssid').disabled = this.value.length > 0;
    });
)HTML";

        if (testing)
        {
            // Reload once the test has finished - the page then shows the
            // result. Keeps retrying while the phone is briefly off this WiFi.
            html += R"HTML(
    (function poll(){
      fetch('/status').then(r => r.json()).then(s => {
        if (s.state === 'testing') setTimeout(poll, 1500);
        else location.replace('/');
      }).catch(() => setTimeout(poll, 1500));
    })();
)HTML";
        }

        html += "  </script>";
        html += PAGE_TAIL;
        return html;
    }

    String currentPage()
    {
        switch (testState)
        {
        case Test::Ok:
            return successPage();
        case Test::Testing:
            return formPage("Testing the connection to <b>" + htmlEscape(form.ssid) +
                                "</b>... this takes up to 15 seconds. If your phone leaves this WiFi for a moment, "
                                "reconnect to <b>" + apName() + "</b> - this page updates by itself.",
                            "info");
        case Test::Failed:
            return formPage(failureMessage, "error");
        default:
            return formPage();
        }
    }

    void saveForm()
    {
        AppStorage::setWifiCredentials(form.ssid, form.password);
        AppStorage::setDeviceConfig(form.deviceName, form.owner);
        AppStorage::setWhatsAppConfig(form.waPhone); // blank is valid (no alerts)
        AppStorage::setDevelopmentDevice(form.devMode);
    }

    void startWifiTest()
    {
        Logger::info(TAG, "Testing WiFi \"" + form.ssid + "\" before saving");
        lastDisconnectReason = 0;
        WiFi.disconnect(false);
        WiFi.begin(form.ssid.c_str(), form.password.c_str());
        testState = Test::Testing;
        testStartMs = millis();
    }

    void updateWifiTest()
    {
        if (testState != Test::Testing)
            return;

        if (WiFi.status() == WL_CONNECTED)
        {
            saveForm();
            testState = Test::Ok;
            restartAtMs = millis() + RESTART_DELAY_MS;
            Logger::info(TAG, "WiFi test passed - details saved, restarting shortly");
            return;
        }

        if (millis() - testStartMs >= WIFI_TEST_TIMEOUT_MS)
        {
            WiFi.disconnect(false); // stop retrying; the setup WiFi stays up
            testState = Test::Failed;
            failureMessage = failureText(lastDisconnectReason, form.ssid);
            Logger::warn(TAG, "WiFi test failed (reason " + String(lastDisconnectReason) + ") - nothing saved");
        }
    }

    void handleRoot()
    {
        server.send(200, "text/html", currentPage());
    }

    void handleScan()
    {
        // Serves the scan done once in run(), before the AP started -
        // scanning live while the AP is already serving clients shares
        // the same radio and is unreliable, which is exactly what
        // produced an empty dropdown before this fix.
        server.send(200, "application/json", cachedScanJson);
    }

    void handleStatus()
    {
        const char *state = "idle";
        if (testState == Test::Testing)
            state = "testing";
        else if (testState == Test::Ok)
            state = "ok";
        else if (testState == Test::Failed)
            state = "failed";
        server.send(200, "application/json", String("{\"state\":\"") + state + "\"}");
    }

    void handleSave()
    {
        if (testState == Test::Testing || testState == Test::Ok)
        {
            server.send(200, "text/html", currentPage()); // a double tap - already on it
            return;
        }

        String ssid = server.arg("ssid_manual");
        if (ssid.length() == 0)
            ssid = server.arg("ssid");

        form.ssid = ssid;
        form.password = server.arg("password");
        form.deviceName = server.arg("devicename");
        form.deviceName.trim();
        form.owner = server.arg("owner");
        form.owner.trim();
        form.waPhone = server.arg("wa_phone");
        form.waPhone.trim();
        form.devMode = server.hasArg("dev_mode");

        testState = Test::Idle;

        if (form.ssid.length() == 0)
        {
            server.send(200, "text/html", formPage("Please choose a WiFi network.", "error"));
            return;
        }

        if (form.deviceName.length() == 0)
        {
            server.send(200, "text/html", formPage("Please enter a device name.", "error"));
            return;
        }

        // Not real email validation, just a sanity check - this value
        // has to exactly match a real Firebase Auth account for control
        // to work at all, so an obviously wrong value is worth catching
        // here rather than producing a silently-locked-out device.
        if (form.owner.indexOf('@') < 1 || form.owner.indexOf('.', form.owner.indexOf('@')) < 0)
        {
            server.send(200, "text/html", formPage("That doesn&rsquo;t look like a valid email - please check it.", "error"));
            return;
        }

        startWifiTest();
        server.send(200, "text/html", currentPage());
    }

    void handleNotFound()
    {
        // Captive portal catch-all: any unmatched path (including the
        // OS's own "is this a captive portal" probe URLs) redirects back
        // to the setup page, which is what triggers the auto-popup on
        // phones when they join the AP.
        server.sendHeader("Location", "/", true);
        server.send(302, "text/plain", "");
    }
}

namespace Provisioning
{
    void run()
    {
        form.ssid = AppStorage::wifiSSID();
        form.deviceName = AppStorage::deviceName();
        form.owner = AppStorage::ownerEmail();
        form.waPhone = AppStorage::whatsAppPhone();
        form.devMode = AppStorage::isDevelopmentDevice();

        // Our own storage keeps the credentials; don't let the WiFi library
        // also write every test attempt to flash.
        WiFi.persistent(false);

        WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info)
                     { lastDisconnectReason = info.wifi_sta_disconnected.reason; },
                     ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

        // Scan BEFORE starting the AP. Scanning while the AP is already
        // live and serving a connected phone shares the same radio and
        // is unreliable on the ESP32 - this was the actual cause of the
        // empty dropdown. A clean STA-mode scan first is much safer.
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        delay(100);

        int n = WiFi.scanNetworks();

        Logger::info(TAG, "WiFi scan found " + String(n > 0 ? n : 0) + " networks");

        if (n > 0)
        {
            String json = "[";
            for (int i = 0; i < n; i++)
            {
                if (i > 0)
                    json += ",";
                json += "\"" + JsonUtil::escape(WiFi.SSID(i)) + "\"";
            }
            json += "]";
            cachedScanJson = json;
        }
        else
        {
            Logger::warn(TAG, "Scan returned nothing (result code " + String(n) + ") - manual entry still available");
            cachedScanJson = "[]";
        }

        WiFi.scanDelete();

        WiFi.mode(WIFI_AP_STA);

        String ssid = apName();
        WiFi.softAP(ssid.c_str());

        IPAddress apIP = WiFi.softAPIP();

        Logger::info(TAG, "Provisioning mode - connect to WiFi \"" + ssid + "\", then visit " + apIP.toString());

        dnsServer.start(DNS_PORT, "*", apIP);

        server.on("/", handleRoot);
        server.on("/scan", handleScan);
        server.on("/status", handleStatus);
        server.on("/save", HTTP_POST, handleSave);
        server.onNotFound(handleNotFound);
        server.begin();

        while (restartAtMs == 0 || (long)(millis() - restartAtMs) < 0)
        {
            dnsServer.processNextRequest();
            server.handleClient();
            updateWifiTest();
            delay(2);
        }

        Logger::info(TAG, "Setup complete - restarting");
        ESP.restart();
    }
}
