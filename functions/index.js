const { onSchedule } = require("firebase-functions/v2/scheduler");
const { onCall, onRequest, HttpsError } = require("firebase-functions/v2/https");
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
      // Snapshot motor state HERE, at first detection - not later, at
      // recovery time. This is the one moment that's actually safe to
      // read it: the device has been silently offline for a while
      // already, so there's no race with its own reconnect sequence
      // the way reading motor/state at recovery time would have (the
      // device republishes its own fresh state within a few seconds
      // of reconnecting - see main.cpp's baselineEstablished gating -
      // so a read at THAT point could easily land after the fresh
      // value overwrote the real pre-outage one). Powers the
      // auto-resume feature (onPowerRestored below) - "was this
      // motor actually running right before the outage" needs to be
      // answered from a value that was frozen before recovery could
      // possibly race it.
      const motor = (data && data.motor) || {};
      await setDedupFlag(deviceId, motor.state);
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
      setDedupFlag: async (deviceId, motorStateAtOutage) => {
        // Multi-path update, not two separate .set() calls - both
        // fields should land together atomically, and this is also
        // just one round-trip instead of two.
        await db.ref(`devices/${deviceId}`).update({
          "status/powerAlertSent": true,
          "motor/stateBeforeOutage": motorStateAtOutage || null,
        });
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
// Skips no-op writes where the value didn't actually change (a
// republish of the same state, which shouldn't happen in practice but
// costs nothing to guard against). Doesn't need a separate "is this
// the very first write" check: before is null for a path that never
// existed, which already differs from any real "RUNNING"/"OFF" value,
// so a brand-new device's first-ever heartbeat naturally passes this
// check rather than being skipped by it - harmless in practice, since
// nobody has push notifications enabled yet for a device that was
// just provisioned seconds ago.
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
      let body = after === "RUNNING" ? "Motor started." : "Motor stopped.";

      // startedVia only ever applies to a transition into RUNNING (see
      // src/cloud.h's comment on remoteStartWasPending() for why a
      // stop can't be attributed the same safe way) - a separate read
      // since this trigger only sees the motor/state leaf itself, not
      // its sibling fields. Absent for OFF transitions and for the
      // rare case a RUNNING transition happened without it (falls
      // back to the plain message, same as before this existed).
      if (after === "RUNNING") {
        const viaSnap = await db.ref(`devices/${deviceId}/motor/startedVia`).once("value");
        const via = viaSnap.val();
        if (via === "remote") {
          body = "Motor started via remote command.";
        } else if (via === "manual") {
          body = "Motor started manually at the panel.";
        }
      }

      const result = await sendPushToDevice(deviceId, `${name} is now ${after}`, body);
      logger.info(`[MotorPush] ${deviceId} (${name}): ${before} -> ${after}, sent to ${result.sent} token(s)`);
    } catch (err) {
      logger.error(`[MotorPush] ${deviceId} (${name}): send failed: ${err}`);
    }
  }
);

// Pure decision logic for auto-resume, separated from the actual
// delayed action (below) so it's unit-testable (see test.js) without
// needing to fake a real multi-minute sleep.
//
// Opt-in and off unless a device's owner explicitly enabled it -
// this restarts a physical motor with no human confirming in the
// moment, so silence (missing/absent settings) always means "do
// nothing," never "assume yes."
function shouldAutoResume(autoResumeSettings, motorStateBeforeOutage) {
  if (!autoResumeSettings || autoResumeSettings.enabled !== true) {
    return false;
  }

  // "Restore previous state," not "always turn on" - only resumes if
  // the motor was actually running right before the outage (the
  // snapshot runWatchdog took at outage detection - see its own
  // comment for why that specific moment, not recovery time, is the
  // only race-free place to have captured this).
  return motorStateBeforeOutage === "RUNNING";
}

// 1-10 minutes, matching the dashboard's own input constraints -
// clamped again here rather than trusting the client-supplied range,
// and falling back to the shortest safe delay (not 0/immediate, not
// NaN) if the stored value is ever missing or malformed.
function clampAutoResumeDelayMinutes(delayMinutes) {
  const n = Number(delayMinutes);
  if (!Number.isFinite(n)) return 1;
  return Math.min(10, Math.max(1, n));
}
module.exports.shouldAutoResume = shouldAutoResume; // exported for test.js
module.exports.clampAutoResumeDelayMinutes = clampAutoResumeDelayMinutes; // exported for test.js

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

    // Auto-resume, entirely separate from the push notification above
    // (which has already gone out either way by this point) - reads
    // this device's own opt-in settings and the motor-state snapshot
    // runWatchdog took at outage detection, not anything live.
    //
    // Deliberately does NOT sleep inline here to wait out the delay -
    // event-handling functions (this trigger type) have a hard 540s
    // (9 min) timeoutSeconds ceiling, confirmed directly by actually
    // trying to load this function with a longer timeout configured
    // (it threw immediately) - a 10-minute requested delay alone
    // already exceeds that cap before any buffer for the work around
    // it, so a single long-running invocation genuinely can't do
    // this. Instead just writes a scheduling record; the actual
    // delayed start is autoResumeWatchdog below - a periodic scheduled
    // function, the same pattern powerWatchdog already uses
    // successfully - checking once a minute for delays that have
    // elapsed, which has no such ceiling since the waiting happens
    // across many short, independent runs instead of one long one.
    try {
      const [autoResumeSnap, stateBeforeSnap] = await Promise.all([
        db.ref(`devices/${deviceId}/autoResume`).once("value"),
        db.ref(`devices/${deviceId}/motor/stateBeforeOutage`).once("value"),
      ]);

      const autoResumeSettings = autoResumeSnap.val();
      const stateBeforeOutage = stateBeforeSnap.val();

      if (shouldAutoResume(autoResumeSettings, stateBeforeOutage)) {
        const delayMinutes = clampAutoResumeDelayMinutes(autoResumeSettings.delayMinutes);
        const dueAt = Date.now() + delayMinutes * 60 * 1000;

        await db.ref(`devices/${deviceId}/autoResume/dueAt`).set(dueAt);
        logger.info(`[AutoResume] ${deviceId} (${name}): was RUNNING before the outage, auto-resume enabled - scheduled for ${delayMinutes}m from now (${new Date(dueAt).toISOString()})`);
      }

      // Clear the snapshot either way, once this outage's recovery has
      // been fully handled - avoids a stale value lingering and being
      // misread at some unrelated future outage if auto-resume gets
      // disabled in between.
      await db.ref(`devices/${deviceId}/motor/stateBeforeOutage`).remove();
    } catch (err) {
      logger.error(`[AutoResume] ${deviceId} (${name}): failed: ${err}`);
    }
  }
);

// Companion to onPowerRestored above - that function only ever
// schedules an auto-resume (writes autoResume/dueAt), it never sends
// the start command itself, since a single event-handling invocation
// can't safely sleep out a delay that can be up to 10 real minutes
// (hard 540s timeout ceiling on that trigger type - see its own
// comment). This is what actually acts once a scheduled delay has
// elapsed - same "run every minute, check every device" shape as
// powerWatchdog, just checking a different condition.
async function runAutoResumeWatchdog({ fetchDevices, sendStartCommand, clearDueAt, nowMs }) {
  const devices = (await fetchDevices()) || {};
  const results = [];

  for (const [deviceId, data] of Object.entries(devices)) {
    const autoResume = (data && data.autoResume) || {};
    const dueAt = autoResume.dueAt;

    if (dueAt === undefined || dueAt === null) {
      continue; // nothing scheduled for this device right now
    }

    if (nowMs < dueAt) {
      continue; // scheduled, but the delay hasn't elapsed yet
    }

    try {
      await sendStartCommand(deviceId);
      results.push(deviceId);
    } catch (err) {
      logger.error(`[AutoResumeWatchdog] ${deviceId}: failed to send start command: ${err}`);
      continue; // don't clear dueAt if the send itself failed - retry next run
    }

    try {
      await clearDueAt(deviceId);
    } catch (err) {
      logger.error(`[AutoResumeWatchdog] ${deviceId}: start sent but failed to clear dueAt (may re-send next run): ${err}`);
    }
  }

  return results; // list of device IDs actually resumed, useful for tests
}
module.exports.runAutoResumeWatchdog = runAutoResumeWatchdog; // exported for test.js

exports.autoResumeWatchdog = onSchedule("every 1 minutes", async () => {
  const db = getDatabase();

  await runAutoResumeWatchdog({
    fetchDevices: async () => {
      const snapshot = await db.ref("devices").once("value");
      return snapshot.val();
    },
    sendStartCommand: async (deviceId) => {
      // Same command/action path a person clicking Start on the
      // dashboard already writes to - the device's own
      // checkCommandConfirmation() (src/cloud.cpp) and the existing
      // failed-start alert both apply automatically, no special-
      // casing needed for this being automated rather than a human
      // click.
      await db.ref(`devices/${deviceId}/command/action`).set("start");
    },
    clearDueAt: async (deviceId) => {
      await db.ref(`devices/${deviceId}/autoResume/dueAt`).remove();
    },
    nowMs: Date.now(),
  });
});

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

// web/flash.html's browser flasher needs to fetch firmware binaries
// from GitHub Releases, but GitHub's release-asset CDN
// (objects.githubusercontent.com, after the redirect) sends no
// Access-Control-Allow-Origin header at all - confirmed directly (curl
// -I against a real release asset shows no CORS header present), not
// assumed. A browser fetch() from a different origin (this dashboard)
// is blocked outright, regardless of the asset being fully public.
// GitHub's own REST API (api.github.com) *is* CORS-enabled, which is
// why flash.html can list releases directly - it's specifically the
// binary download endpoint that isn't.
//
// This function is the fix: CORS is a browser-enforced restriction,
// it doesn't apply to a server fetching another server at all, so this
// fetches the asset itself (no CORS problem here) and returns it with
// its own Access-Control-Allow-Origin header set. GitHub stays the
// single real source of firmware binaries - this only ever relays
// bytes, never stores or modifies them.
const FIRMWARE_ASSET_ALLOWLIST = ["firmware.bin", "bootloader.bin", "partitions.bin", "boot_app0.bin"];
const FIRMWARE_TAG_PATTERN = /^v\d+\.\d+\.\d+$/;

// Pure validation, separated from the HTTP trigger below so it can be
// unit tested directly (see test.js) without needing to mock an
// Express request/response pair.
function isValidFirmwareAssetRequest(tag, file) {
  return typeof tag === "string" && FIRMWARE_TAG_PATTERN.test(tag) &&
         typeof file === "string" && FIRMWARE_ASSET_ALLOWLIST.includes(file);
}
module.exports.isValidFirmwareAssetRequest = isValidFirmwareAssetRequest; // exported for test.js

// Deliberately public/unauthenticated - the whole point is helping
// someone set up their very first device, plausibly before they have
// (or before a friend helping them has) any Furrow sign-in at all.
// Gating this behind auth would add friction to exactly the moment
// it's supposed to remove it, for data that's already fully public
// on GitHub's own release page regardless. tag/file are both
// strictly validated against fixed patterns before being used to
// build the outbound URL - never passed through directly - so this
// can't become an open proxy for arbitrary GitHub URLs.
//
// invoker: "public" is explicit, not left to the CLI's default -
// found genuinely conflicting accounts of whether Firebase's tooling
// makes a new onRequest function callable by an anonymous browser by
// default or not (a Firebase team member's own forum comment says
// yes; another real-world report says the opposite happened for
// them). Rather than deploy and find out which one was true this
// time via a silent 403 in flash.html, this removes the ambiguity
// outright.
exports.firmwareProxy = onRequest({ invoker: "public" }, async (req, res) => {
  res.set("Access-Control-Allow-Origin", "*");

  const tag = req.query.tag;
  const file = req.query.file;

  if (!isValidFirmwareAssetRequest(tag, file)) {
    res.status(400).send("Invalid tag or file");
    return;
  }

  const url = `https://github.com/ultranoob-5/furrow/releases/download/${tag}/${file}`;

  try {
    const upstream = await fetch(url);

    if (!upstream.ok) {
      logger.warn(`[FirmwareProxy] upstream ${upstream.status} for ${tag}/${file}`);
      res.status(upstream.status).send(`GitHub returned ${upstream.status} for this release asset`);
      return;
    }

    res.set("Content-Type", "application/octet-stream");
    res.send(Buffer.from(await upstream.arrayBuffer()));
  } catch (err) {
    logger.error(`[FirmwareProxy] fetch failed for ${tag}/${file}: ${err}`);
    res.status(502).send("Failed to fetch firmware from GitHub");
  }
});
