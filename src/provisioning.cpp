#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>

#include "provisioning.h"
#include "storage.h"
#include "logger.h"

namespace
{
    constexpr const char *TAG = "Provisioning";
    constexpr byte DNS_PORT = 53;

    DNSServer dnsServer;
    WebServer server(80);

    bool credentialsSubmitted = false;
    String cachedScanJson = "[]";

    String apName()
    {
        String mac = WiFi.macAddress(); // "AA:BB:CC:DD:EE:FF"
        mac.replace(":", "");
        String suffix = mac.substring(mac.length() - 4);
        suffix.toUpperCase();
        return "Furrow-Setup-" + suffix;
    }

    String htmlPage(const String &statusMessage = "")
    {
        String html = R"HTML(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Furrow Setup</title>
<style>
  body { font-family: -apple-system, sans-serif; background:#14161A; color:#ECE9E2; margin:0; padding:24px; }
  .card { max-width:360px; margin:0 auto; background:#1B1E23; border:1px solid #2E333B; border-radius:12px; padding:24px; }
  h1 { font-size:18px; margin:0 0 4px; }
  p.sub { color:#868C97; font-size:13px; margin:0 0 20px; }
  label { display:block; font-size:12px; color:#868C97; margin:14px 0 6px; text-transform:uppercase; letter-spacing:0.06em; }
  select, input { width:100%; box-sizing:border-box; padding:10px 12px; border-radius:8px; border:1px solid #2E333B; background:#21252B; color:#ECE9E2; font-size:14px; }
  button { width:100%; margin-top:20px; padding:12px 0; border:none; border-radius:8px; background:#E8A33D; color:#241804; font-weight:700; font-size:14px; cursor:pointer; }
  .status { margin-top:14px; font-size:13px; color:#49B675; }
</style>
</head>
<body>
  <div class="card">
    <h1>Furrow</h1>
    <p class="sub">Connect this device to your WiFi network</p>
    <form method="POST" action="/save">
      <label>WiFi Network</label>
      <select name="ssid" id="ssid"><option value="">Scanning...</option></select>
      <label>Or type the network name manually (use this if yours isn't listed above)</label>
      <input type="text" name="ssid_manual" id="ssid_manual" placeholder="Leave blank to use the dropdown">
      <label>Password</label>
      <input type="password" name="password" placeholder="WiFi password">
      <label>Device Name</label>
      <input type="text" name="devicename" placeholder="e.g. Farm Pump" maxlength="40">
      <label>Owner Email</label>
      <input type="email" name="owner" placeholder="you@example.com" maxlength="60">
      <label>WhatsApp Alerts (optional)</label>
      <input type="text" name="wa_phone" placeholder="Your number, digits only e.g. 919876543210" maxlength="20">
      <button type="submit">Save &amp; Connect</button>
    </form>
    <div class="status">)HTML";
        html += statusMessage;
        html += R"HTML(</div>
  </div>
  <script>
    fetch('/scan').then(r => r.json()).then(list => {
      const sel = document.getElementById('ssid');
      sel.innerHTML = '';
      if (list.length === 0) {
        const opt = document.createElement('option');
        opt.value = ''; opt.textContent = 'No networks found - type yours below';
        sel.appendChild(opt);
        return;
      }
      list.forEach(ssid => {
        const opt = document.createElement('option');
        opt.value = ssid; opt.textContent = ssid;
        sel.appendChild(opt);
      });
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
  </script>
</body>
</html>
)HTML";
        return html;
    }

    void handleRoot()
    {
        server.send(200, "text/html", htmlPage());
    }

    void handleScan()
    {
        // Serves the scan done once in run(), before the AP started -
        // scanning live while the AP is already serving clients shares
        // the same radio and is unreliable, which is exactly what
        // produced an empty dropdown before this fix.
        server.send(200, "application/json", cachedScanJson);
    }

    void handleSave()
    {
        String ssid = server.arg("ssid_manual");
        if (ssid.length() == 0)
            ssid = server.arg("ssid");

        String password = server.arg("password");
        String deviceName = server.arg("devicename");
        String owner = server.arg("owner");
        String waPhone = server.arg("wa_phone");

        if (ssid.length() == 0)
        {
            server.send(200, "text/html", htmlPage("Please choose a WiFi network."));
            return;
        }

        if (deviceName.length() == 0)
        {
            server.send(200, "text/html", htmlPage("Please enter a device name."));
            return;
        }

        // Not real email validation, just a sanity check - this value
        // has to exactly match a real Firebase Auth account for control
        // to work at all, so an obviously wrong value is worth catching
        // here rather than producing a silently-locked-out device.
        if (owner.indexOf('@') < 1 || owner.indexOf('.', owner.indexOf('@')) < 0)
        {
            server.send(200, "text/html", htmlPage("That doesn't look like a valid email - please check it."));
            return;
        }

        AppStorage::setWifiCredentials(ssid, password);
        AppStorage::setDeviceConfig(deviceName, owner);

        // Genuinely optional - blank is a valid choice (no alerts).
        if (waPhone.length() > 0)
        {
            AppStorage::setWhatsAppConfig(waPhone);
        }

        server.send(200, "text/html", htmlPage("Saved. Restarting and connecting..."));

        credentialsSubmitted = true;
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
                json += "\"" + WiFi.SSID(i) + "\"";
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
        server.on("/save", HTTP_POST, handleSave);
        server.onNotFound(handleNotFound);
        server.begin();

        while (!credentialsSubmitted)
        {
            dnsServer.processNextRequest();
            server.handleClient();
            delay(2);
        }

        Logger::info(TAG, "Credentials received - restarting");

        delay(1500); // let the confirmation page actually finish sending
        ESP.restart();
    }
}
