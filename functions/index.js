const { onSchedule } = require("firebase-functions/v2/scheduler");
const { onCall, HttpsError } = require("firebase-functions/v2/https");
const { onValueWritten } = require("firebase-functions/v2/database");
const { defineSecret } = require("firebase-functions/params");
const { logger } = require("firebase-functions");
const admin = require("firebase-admin");
const { getDatabase } = require("firebase-admin/database");
const { getMessaging } = require("firebase-admin/messaging");

admin.initializeApp();

// The actual RTDB instance lives in asia-southeast1 (see .firebaserc/
// databaseURL), not the us-central1 the other functions in this file
// default to. Realtime Database triggers specifically (unlike
// scheduled/callable functions) must be deployed in the same region
// as the database itself or the deploy fails outright - confirmed
// against Firebase's own docs. Scheduled/callable functions have no
// such constraint, which is why only the two onValueWritten triggers
// below need this.
const RTDB_REGION = "asia-southeast1";

const whapiToken = defineSecret("WHAPI_TOKEN");

const OFFLINE_THRESHOLD_MS = 30000;

// Pure decision + action logic, separated from the scheduled trigger so
// it can be unit tested with fake fetchDevices/sendWhatsApp/sendPush/
// setDedupFlag functions instead of needing the Firebase emulator -
// see test.js.
//
// WhatsApp and push are independent channels, not one gating the
// other: a device with push tokens but no WhatsApp phone configured
// (or vice versa) still gets alerted through whichever channel it
// actually has. The dedup flag only gets set once at least one
// channel actually succeeded, so a device with neither configured
// just keeps logging and retrying harmlessly every run, same as
// before this existed.
async function runWatchdog({ fetchDevices, sendWhatsApp, sendPush, setDedupFlag, nowMs }) {
  const devices = (await fetchDevices()) || {};
  const results = [];

  for (const [deviceId, data] of Object.entries(devices)) {
    const status = (data && data.status) || {};
    const lastSeen = status.lastSeen;
    const name = status.name || deviceId;
    const phone = status.whatsappPhone;
    const alreadyAlerted = status.powerAlertSent === true;

    if (lastSeen === undefined || lastSeen === null) {
      continue; // never reported at all - nothing to compare against
    }

    if (alreadyAlerted) {
      continue; // already sent for this outage - skip immediately, before
      // even computing ageMs, so a long-dead device costs one cheap
      // property check per run, not a real evaluation. (Coming back
      // online clears this on its own - see Cloud::publishDevice() in
      // src/cloud.cpp, which republishes powerAlertSent:false on every
      // heartbeat while the device is reporting normally, no watchdog
      // involvement needed.)
    }

    const ageMs = nowMs - lastSeen;

    if (ageMs < OFFLINE_THRESHOLD_MS) {
      continue; // still within normal heartbeat range
    }

    const offlineSeconds = Math.floor(ageMs / 1000);
    let anySent = false;

    if (phone) {
      try {
        await sendWhatsApp(phone, `\u26a0\ufe0f ${name} appears to have lost power - no contact for over 30s`);
        anySent = true;
      } catch (err) {
        logger.error(`  WhatsApp send failed: ${err}`);
      }
    } else {
      logger.info(`${deviceId} (${name}): offline ${offlineSeconds}s, no WhatsApp recipient configured - skipping WhatsApp`);
    }

    if (sendPush) {
      try {
        const pushResult = await sendPush(deviceId, `${name} lost power`, `No contact for over ${offlineSeconds}s`);
        if (pushResult && pushResult.sent > 0) anySent = true;
      } catch (err) {
        logger.error(`  Push send failed: ${err}`);
      }
    }

    if (!anySent) {
      logger.info(`${deviceId} (${name}): offline ${offlineSeconds}s, no channel available or all sends failed - will retry next run`);
      continue; // don't mark alerted - neither channel worked (or neither is configured)
    }

    logger.info(`${deviceId} (${name}): offline ${offlineSeconds}s - alerted`);

    try {
      await setDedupFlag(deviceId);
    } catch (err) {
      logger.error(`  Failed to set dedup flag (may re-alert next run): ${err}`);
    }

    results.push(deviceId);
  }

  return results; // list of device IDs actually alerted, useful for tests
}

async function sendWhatsAppReal(phone, message) {
  const res = await fetch("https://gate.whapi.cloud/messages/text", {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      Authorization: `Bearer ${whapiToken.value()}`,
    },
    body: JSON.stringify({ to: phone, body: message }),
  });

  if (!res.ok) {
    const text = await res.text();
    throw new Error(`HTTP ${res.status}: ${text}`);
  }
}

// Sends a push to every token registered for a device (devices/{id}/
// fcmTokens - see database.rules.example.json, owner-writable, written
// by the dashboard when a person enables notifications on that
// device's own panel). Multiple tokens are expected and fine - e.g.
// two family members each enabling notifications for the same pump
// from their own phones.
//
// Prunes any token FCM reports as dead (messaging/registration-token-
// not-registered - happens when a browser clears site data,
// uninstalls the PWA, etc.) so this list doesn't grow forever with
// entries that can never succeed again.
async function sendPushToDevice(deviceId, title, body) {
  const db = getDatabase();
  const tokensSnap = await db.ref(`devices/${deviceId}/fcmTokens`).once("value");
  const tokensObj = tokensSnap.val() || {};
  const entries = Object.entries(tokensObj); // [ [pushId, token], ... ]

  if (entries.length === 0) {
    return { sent: 0, pruned: 0 };
  }

  const response = await getMessaging().sendEachForMulticast({
    tokens: entries.map(([, token]) => token),
    // data, not notification - see dashboard.html/sw.js's handlers for
    // why: a notification-field message gets auto-displayed by the
    // browser/SDK in addition to our own onMessage/onBackgroundMessage
    // handler explicitly calling showNotification(), producing two
    // separate notifications for one event (confirmed on real
    // hardware - one with the real Furrow icon from our handler, one
    // generic/iconless from the SDK's own fallback display). A
    // data-only message has nothing for the browser to auto-display,
    // so our handler is the only thing that ever shows anything.
    data: { title, body },
  });

  const deadPushIds = [];
  response.responses.forEach((r, i) => {
    if (!r.success && r.error && r.error.code === "messaging/registration-token-not-registered") {
      deadPushIds.push(entries[i][0]);
    }
  });

  if (deadPushIds.length > 0) {
    const updates = {};
    for (const id of deadPushIds) updates[id] = null;
    await db.ref(`devices/${deviceId}/fcmTokens`).update(updates);
  }

  return { sent: response.successCount, pruned: deadPushIds.length };
}

// Runs outside any device entirely - this is what makes it able to
// detect a device that's genuinely lost power (and therefore can't
// report its own absence itself). See functions/test.js for the
// decision logic's test coverage, and src/cloud.cpp for how the
// per-device whatsappPhone gets published and how the powerAlertSent
// dedup flag resets automatically once a device is back.
//
// Runs on Firebase's own infrastructure (Google Cloud), not GitHub
// Actions' shared runner IP pool - moved here specifically to rule out
// (or confirm) whether that shared IP range was the cause of Whapi
// rejecting the equivalent GitHub Actions version with a 403.
exports.powerWatchdog = onSchedule(
  {
    schedule: "every 1 minutes",
    secrets: [whapiToken],
  },
  async () => {
    const db = getDatabase();

    await runWatchdog({
      fetchDevices: async () => {
        const snapshot = await db.ref("devices").once("value");
        return snapshot.val();
      },
      sendWhatsApp: sendWhatsAppReal,
      sendPush: sendPushToDevice,
      setDedupFlag: async (deviceId) => {
        await db.ref(`devices/${deviceId}/status/powerAlertSent`).set(true);
      },
      nowMs: Date.now(),
    });
  }
);

module.exports.runWatchdog = runWatchdog; // exported for test.js

// Fires a push whenever a device's motor actually changes state - see
// src/cloud.cpp's publishMotor(), which writes devices/{id}/motor/state
// as "RUNNING" or "OFF" any time CurrentSensor's real feedback flips
// (physical button press, remote command, or the motor tripping/
// stopping on its own - all of them, not just remote commands, since
// this is driven by real current sensing, not the last command sent).
//
// Skips the very first write (event.data.before doesn't exist) so
// provisioning/first-boot doesn't fire a spurious alert, and skips
// no-op writes where the value didn't actually change (a republish of
// the same state, which shouldn't happen in practice but costs
// nothing to guard against).
exports.onMotorStateChanged = onValueWritten(
  { ref: "/devices/{deviceId}/motor/state", region: RTDB_REGION },
  async (event) => {
    const before = event.data.before.val();
    const after = event.data.after.val();

    if (before === after || after === null) {
      return;
    }

    const deviceId = event.params.deviceId;
    const db = getDatabase();
    const nameSnap = await db.ref(`devices/${deviceId}/status/name`).once("value");
    const name = nameSnap.val() || deviceId;

    try {
      const body = after === "RUNNING" ? "Motor started." : "Motor stopped.";
      const result = await sendPushToDevice(deviceId, `${name} is now ${after}`, body);
      logger.info(`[MotorPush] ${deviceId} (${name}): ${before} -> ${after}, sent to ${result.sent} token(s)`);
    } catch (err) {
      logger.error(`[MotorPush] ${deviceId} (${name}): send failed: ${err}`);
    }
  }
);

// Fires a push when a device recovers from a flagged power-loss
// outage - watches powerAlertSent's true -> false transition, which
// Cloud::publishDevice() (src/cloud.cpp) already sets automatically
// the moment a device resumes normal heartbeats after being flagged
// by runWatchdog above. No new firmware logic needed - this is purely
// a new observer on a signal that already existed.
//
// Deliberately does NOT cover a brief WiFi blip under the 30s
// threshold reconnecting (powerAlertSent is never set for those in
// the first place, since runWatchdog only flags genuinely extended
// outages) - that's consistent with treating this as a real power
// event, not every momentary WiFi hiccup while still powered.
exports.onPowerRestored = onValueWritten(
  { ref: "/devices/{deviceId}/status/powerAlertSent", region: RTDB_REGION },
  async (event) => {
    const before = event.data.before.val();
    const after = event.data.after.val();

    if (before !== true || after !== false) {
      return;
    }

    const deviceId = event.params.deviceId;
    const db = getDatabase();
    const nameSnap = await db.ref(`devices/${deviceId}/status/name`).once("value");
    const name = nameSnap.val() || deviceId;

    try {
      const result = await sendPushToDevice(deviceId, `${name} power restored`, "Back online after a power loss.");
      logger.info(`[PowerRestoredPush] ${deviceId} (${name}): sent to ${result.sent} token(s)`);
    } catch (err) {
      logger.error(`[PowerRestoredPush] ${deviceId} (${name}): send failed: ${err}`);
    }
  }
);

// Fires a push when a remote start/stop command doesn't actually take
// effect within 30s - see src/cloud.cpp's checkCommandConfirmation(),
// which is what writes devices/{id}/motor/commandFailure in the first
// place (compares the command's expected outcome against real CT
// current feedback, not just whether the relay pulse was sent).
// WhatsApp already went out directly from the device itself by the
// time this fires - firmware has its own Whapi credential and doesn't
// need to round-trip through a Cloud Function for that channel; this
// function only ever handles the push side.
//
// Deliberately remote-command-only, same limitation as the firmware
// side: a physical button press at the panel never routes through the
// device's own command handling at all, so there's no "expected
// outcome" recorded anywhere for this to compare against for that
// case.
exports.onMotorCommandFailed = onValueWritten(
  { ref: "/devices/{deviceId}/motor/commandFailure", region: RTDB_REGION },
  async (event) => {
    const after = event.data.after.val();
    if (!after || !after.action) {
      return;
    }

    const deviceId = event.params.deviceId;
    const db = getDatabase();
    const nameSnap = await db.ref(`devices/${deviceId}/status/name`).once("value");
    const name = nameSnap.val() || deviceId;

    const body = after.action === "start"
      ? "No current detected 30s after the start command - check the panel."
      : "Motor still drawing current 30s after the stop command - check the panel.";

    try {
      const result = await sendPushToDevice(deviceId, `${name} failed to ${after.action}`, body);
      logger.info(`[CommandFailedPush] ${deviceId} (${name}): ${after.action} failure, sent to ${result.sent} token(s)`);
    } catch (err) {
      logger.error(`[CommandFailedPush] ${deviceId} (${name}): send failed: ${err}`);
    }
  }
);

// Manual test trigger, called from a device panel's "Send test
// notification" button once notifications are enabled for that
// device - lets someone confirm push actually reaches this browser
// right after enabling, instead of waiting for a real event (power
// loss, motor state change, etc.). Not itself part of any real alert
// path - those are the RTDB-triggered functions below/above
// (powerWatchdog, onMotorStateChanged, onPowerRestored,
// onMotorCommandFailed) - this one only ever fires when a person
// explicitly clicks the test button.
//
// Any signed-in owner can call this - deliberately not restricted
// further, since it never touches device data, only sends a push to
// whatever token the caller's own browser just generated for itself.
exports.sendTestNotification = onCall(async (request) => {
  if (!request.auth) {
    throw new HttpsError("unauthenticated", "Sign in first.");
  }

  const token = request.data && request.data.token;
  if (!token || typeof token !== "string") {
    throw new HttpsError("invalid-argument", "Missing FCM token.");
  }

  const title = (request.data && request.data.title) || "Furrow test";
  const body = (request.data && request.data.body) ||
    "If you see this, push notifications work.";

  try {
    const messageId = await getMessaging().send({
      token,
      // data, not notification - see sendPushToDevice()'s comment for
      // why (avoids a duplicate auto-displayed notification alongside
      // our own handler's).
      data: { title, body },
    });

    logger.info(`[PushTest] sent to ${request.auth.token.email}: ${messageId}`);
    return { ok: true, messageId };
  } catch (err) {
    logger.error(`[PushTest] send failed: ${err.message}`);
    throw new HttpsError("internal", err.message);
  }
});
