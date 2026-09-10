#pragma once

// Cloud connects the controller to Firebase Realtime Database.
//
// RTDB layout (rooted at /devices/{DEVICE_ID}):
//   status/          device heartbeat: id, name, firmware, ip, rssi, online, uptime
//   motor/           motor status: state ("RUNNING" | "OFF"), updatedAt,
//                    startedVia ("remote" | "manual" - only present on the
//                    specific publish that caught a transition into
//                    RUNNING, see publishMotor()'s comment)
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

    // startedVia and stoppedVia are optional and deliberately narrow:
    // pass "remote", "manual", "auto-resume", or "schedule" from the
    // exact call site that just caught a state transition (see main.cpp),
    // never from the routine 10s heartbeat republish. Omitted (nullptr,
    // the default) publishes the plain {state, updatedAt} shape - passing
    // it also means database.set()'s full-replace semantics naturally
    // clear out any previous startedVia/stoppedVia value on the very next
    // publish that doesn't repeat it, so a stale tag never lingers.
    void publishMotor(const char *startedVia = nullptr, const char *stoppedVia = nullptr);

    // True if a remote "start" command was dispatched and is still
    // awaiting CT-current confirmation at the moment this is called -
    // see checkCommandConfirmation() in cloud.cpp.
    bool remoteStartWasPending();

    // True if a remote "stop" command was dispatched and is still
    // awaiting CT-current confirmation at the moment this is called.
    bool remoteStopWasPending();

    // Auto-resume state inspection and cancellation
    bool isAutoResumePending();
    bool autoResumeStartWasPending();
    void cancelAutoResume();

    // Irrigation schedule state inspection
    bool scheduleStartWasPending();
    bool scheduleStopWasPending();
};

extern Cloud cloud;