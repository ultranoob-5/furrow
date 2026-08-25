#pragma once

// Cloud connects the controller to Firebase Realtime Database.
//
// RTDB layout (rooted at /devices/{DEVICE_ID}):
//   status/          device heartbeat: id, name, firmware, ip, rssi, online, uptime
//   motor/           motor status: state ("RUNNING" | "OFF"), updatedAt
//   command/action   remote command written by the dashboard: "start" | "stop" | "none"
//
// The device authenticates as a real, shared device account (Email/
// Password sign-in - see UserAuth deviceAuth in cloud.cpp and step 2 of
// SETUP.md), not anonymously: this is what lets RTDB rules tell "this
// firmware" apart from an unauthenticated stranger, since anonymous
// auth can't be distinguished that way. It listens to command/action via
// a realtime stream, so start/stop commands are applied as soon as they
// are written, without polling.
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