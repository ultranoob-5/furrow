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

    // startedVia is optional and deliberately narrow: pass "remote" or
    // "manual" only from the exact call site that just caught a
    // transition into RUNNING (see main.cpp), never from the routine
    // 10s heartbeat republish or a transition into OFF - see
    // remoteStartWasPending()'s comment for why this isn't done for
    // stops too. Omitted (nullptr, the default) publishes the same
    // plain {state, updatedAt} shape as before this existed - passing
    // it also means database.set()'s full-replace semantics naturally
    // clear out any previous startedVia value on the very next publish
    // that doesn't repeat it, so a stale tag never lingers.
    void publishMotor(const char *startedVia = nullptr);

    // True if a remote "start" command was dispatched and is still
    // awaiting CT-current confirmation at the moment this is called -
    // see checkCommandConfirmation() in cloud.cpp. Meant to be checked
    // at the exact instant a transition into RUNNING is detected
    // (main.cpp, before Cloud::loop() runs later in the same
    // iteration and would otherwise clear it) - if this is false right
    // then, nothing else could have caused a real motor start except
    // the physical button at the panel, since a motor can't start
    // itself the way it can stop for several other reasons (power
    // loss, a trip, an overload) - that asymmetry is why this exists
    // for starts only, never stops.
    bool remoteStartWasPending();
};

extern Cloud cloud;