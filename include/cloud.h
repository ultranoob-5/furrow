#pragma once

// Cloud connects the controller to Firebase Realtime Database.
//
// RTDB layout (rooted at /devices/{DEVICE_ID}):
//   status/          device heartbeat: id, name, firmware, ip, rssi, online, uptime
//   motor/           motor status: state ("RUNNING" | "OFF"), updatedAt
//   command/action   remote command written by the dashboard: "start" | "stop" | "none"
//
// The device authenticates anonymously (requires Anonymous sign-in to be
// enabled in the Firebase console) and listens to command/action via a
// realtime stream, so start/stop commands are applied as soon as they are
// written, without polling.
class Cloud
{
public:
    void begin();

    void loop();

    bool connected();

    void publishDevice();

    void publishMotor();
};

extern Cloud cloud;