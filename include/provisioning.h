#pragma once

// Provisioning runs a temporary WiFi access point + captive portal so
// this device can be given real WiFi credentials from a phone or laptop
// browser - no app, no BLE, no recompiling firmware. Works the same way
// on iPhone, Android, and desktop, since it's just a web page.
//
// Flow: the ESP32 broadcasts its own WiFi network. Connecting to it and
// opening any website auto-redirects to a small setup page (or the user
// visits 192.168.4.1 directly). They pick their real WiFi network and
// enter its password; the device tries it first, and only saves and
// restarts once it has actually connected - a wrong password shows an
// error on the page instead of leaving the device stuck off the network.
namespace Provisioning
{
    // Starts the AP + captive portal and blocks, handling requests,
    // until working credentials have been submitted, tested and saved to
    // Storage. Restarts the device itself once done - does not return.
    void run();
}
