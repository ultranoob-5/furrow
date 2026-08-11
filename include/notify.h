#pragma once

#include <Arduino.h>

// Sends WhatsApp alerts via Whapi.cloud (https://whapi.cloud), through
// one shared sender number/channel for every device (credential lives
// in secrets.h). Entirely optional per-device - if AppStorage has no
// recipient phone number saved, sendWhatsApp() silently does nothing
// rather than erroring, since most devices won't have this configured
// and that's a perfectly valid choice.
namespace Notify
{
    // Fire-and-forget: makes a best-effort HTTP request and returns.
    // Failure (no recipient configured, network issue, Whapi rejecting
    // the request) is logged but never blocks or crashes the caller -
    // an alert failing to send should never be able to affect motor
    // control or anything else this firmware does.
    void sendWhatsApp(const String &message);
}
