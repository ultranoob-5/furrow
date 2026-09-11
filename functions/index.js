const { onSchedule } = require("firebase-functions/v2/scheduler");
const { onCall, onRequest, HttpsError } = require("firebase-functions/v2/https");
const { onValueWritten } = require("firebase-functions/v2/database");
const { defineSecret } = require("firebase-functions/params");
const { logger } = require("firebase-functions");
const admin = require("firebase-admin");
const { getDatabase } = require("firebase-admin/database");
const { getMessaging } = require("firebase-admin/messaging");
const { Readable } = require("stream");

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
    const motor = (data && data.motor) || {};
    const wasRunning = (motor.state === "RUNNING");

    // Only notify power loss when the motor was actively RUNNING before the outage.
    // If the pump was already OFF, the power cut does not interrupt irrigation,
    // avoiding nuisance alerts when mains power flickers on an idle pump.
    if (!wasRunning) {
      logger.info(`${deviceId} (${name}): offline ${offlineSeconds}s, but motor was not RUNNING (${motor.state || "OFF"}) - skipping power loss alert`);
      continue;
    }

    let anySent = false;

    if (phone) {
      try {
        await sendWhatsApp(phone, `\u26a0\ufe0f Power Cut Alert! Electricity lost at ${name}. Motor is OFF.`);
        anySent = true;
      } catch (err) {
        logger.error(`  WhatsApp send failed: ${err}`);
      }
    } else {
      logger.info(`${deviceId} (${name}): offline ${offlineSeconds}s, no WhatsApp recipient configured - skipping WhatsApp`);
    }

    if (sendPush) {
      try {
        const pushResult = await sendPush(deviceId, `\u26a0\ufe0f ${name}: Power Cut`, `Power lost at ${name}. Motor is OFF.`);
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
    data: {
      title,
      body,
      deviceId,
      url: `./dashboard.html?device=${encodeURIComponent(deviceId)}`,
      click_action: `./dashboard.html?device=${encodeURIComponent(deviceId)}`,
    },
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
        // Multi-path update, not two separate .set() calls - all
        // fields land together atomically in one round-trip.
        // Sets motor/state to "OFF" in the database so the dashboard
        // reflects that the pump is no longer running while unpowered,
        // while preserving motor/stateBeforeOutage so autoResume knows
        // to resume when power is restored.
        const safeState = (motorStateAtOutage === "RUNNING" || motorStateAtOutage === "OFF")
          ? motorStateAtOutage
          : null;
        await db.ref(`devices/${deviceId}`).update({
          "status/powerAlertSent": true,
          "motor/stateBeforeOutage": safeState,
          "motor/state": "OFF",
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
// Sends both a Push notification (FCM) and a WhatsApp message to the device recipient.
// Pairs every cloud messaging alert with a matching WhatsApp alert so elder users
// receive instant, clear updates on both their phone lock screen and WhatsApp chat.
async function sendAlertToDevice({ deviceId, name, title, body, whatsappMessage }) {
  const db = getDatabase();

  const pushPromise = sendPushToDevice(deviceId, title, body).catch((err) => {
    logger.error(`[Push] Failed for ${deviceId} (${name}): ${err}`);
    return { sent: 0, pruned: 0 };
  });

  const whatsappPromise = (async () => {
    try {
      const phoneSnap = await db.ref(`devices/${deviceId}/status/whatsappPhone`).once("value");
      const phone = phoneSnap.val();
      if (phone) {
        await sendWhatsAppReal(phone, whatsappMessage || body);
        logger.info(`[WhatsApp] Alert sent to ${phone} for ${deviceId} (${name})`);
      } else {
        logger.info(`[WhatsApp] No phone configured for ${deviceId} (${name}) - skipped`);
      }
    } catch (err) {
      logger.error(`[WhatsApp] Failed for ${deviceId} (${name}): ${err}`);
    }
  })();

  await Promise.all([pushPromise, whatsappPromise]);
}

// Fires whenever a device's motor changes state (RUNNING or OFF) - catches remote
// commands from the web dashboard, physical starter panel button presses,
// scheduled on/off runs, and auto-resume. Sends both Push and WhatsApp notifications
// in simple, clear, elder-friendly wording with visual status emojis.
exports.onMotorStateChanged = onValueWritten(
  { ref: "/devices/{deviceId}/motor/state", region: RTDB_REGION, secrets: [whapiToken] },
  async (event) => {
    const before = event.data.before.val();
    const after = event.data.after.val();

    if (before === after || after === null) {
      return;
    }

    const deviceId = event.params.deviceId;
    const db = getDatabase();

    try {
      let title = "";
      let message = "";
      let name = deviceId;

      if (after === "RUNNING") {
        const [nameSnap, viaSnap] = await Promise.all([
          db.ref(`devices/${deviceId}/status/name`).once("value"),
          db.ref(`devices/${deviceId}/motor/startedVia`).once("value"),
        ]);
        name = nameSnap.val() || deviceId;
        const via = viaSnap.val();

        title = `\u{1f7e2} ${name} turned ON`;
        if (via === "remote") {
          message = `\u{1f7e2} ${name} turned ON from the mobile app / website.`;
        } else if (via === "manual") {
          message = `\u{1f7e2} ${name} turned ON manually at the starter panel.`;
        } else if (via === "auto-resume") {
          message = `\u{1f7e2} ${name} turned ON automatically (power restored).`;
        } else if (via === "schedule") {
          message = `\u{1f7e2} ${name} turned ON by daily schedule.`;
        } else {
          message = `\u{1f7e2} ${name} turned ON.`;
        }
      } else {
        const [nameSnap, viaSnap] = await Promise.all([
          db.ref(`devices/${deviceId}/status/name`).once("value"),
          db.ref(`devices/${deviceId}/motor/stoppedVia`).once("value"),
        ]);
        name = nameSnap.val() || deviceId;
        const via = viaSnap.val();

        title = `\u{1f534} ${name} turned OFF`;
        if (via === "remote") {
          message = `\u{1f534} ${name} turned OFF from the mobile app / website.`;
        } else if (via === "schedule") {
          message = `\u{1f534} ${name} turned OFF by daily schedule.`;
        } else if (via === "manual") {
          message = `\u{1f534} ${name} turned OFF manually at the starter panel.`;
        } else {
          message = `\u{1f534} ${name} turned OFF.`;
        }
      }

      await sendAlertToDevice({ deviceId, name, title, body: message, whatsappMessage: message });
      logger.info(`[MotorAlert] ${deviceId} (${name}): ${before} -> ${after}`);
    } catch (err) {
      logger.error(`[MotorAlert] ${deviceId} error: ${err}`);
    }
  }
);


// Fires a push and WhatsApp alert when a device recovers from a flagged power-loss
// outage - watches powerAlertSent's true -> false transition.
exports.onPowerRestored = onValueWritten(
  { ref: "/devices/{deviceId}/status/powerAlertSent", region: RTDB_REGION, secrets: [whapiToken] },
  async (event) => {
    const before = event.data.before.val();
    const after = event.data.after.val();

    if (before !== true || after !== false) {
      return;
    }

    const deviceId = event.params.deviceId;
    const db = getDatabase();

    try {
      const [nameSnap, autoResumeSnap, stateBeforeSnap] = await Promise.all([
        db.ref(`devices/${deviceId}/status/name`).once("value"),
        db.ref(`devices/${deviceId}/autoResume`).once("value"),
        db.ref(`devices/${deviceId}/motor/stateBeforeOutage`).once("value"),
      ]);

      const name = nameSnap.val() || deviceId;
      const autoResume = autoResumeSnap.val() || {};
      const stateBefore = stateBeforeSnap.val();

      let title = `\u26a1 ${name}: Power Restored`;
      let message = `\u26a1 Power is back! ${name} is connected and ready.`;
      if (autoResume.enabled === true && stateBefore === "RUNNING") {
        const delay = autoResume.delayMinutes || 5;
        message = `\u26a1 Power is back! ${name} motor will turn ON in ${delay} min.`;
      }

      await sendAlertToDevice({ deviceId, name, title, body: message, whatsappMessage: message });
      logger.info(`[PowerRestoredAlert] ${deviceId} (${name}): alert sent`);
    } catch (err) {
      logger.error(`[PowerRestoredAlert] ${deviceId}: send failed: ${err}`);
    }
  }
);


// Fires push and WhatsApp alerts when a remote start/stop command doesn't take
// effect within 30s. Plain, non-technical alert guiding the user to check the panel.
exports.onMotorCommandFailed = onValueWritten(
  { ref: "/devices/{deviceId}/motor/commandFailure", region: RTDB_REGION, secrets: [whapiToken] },
  async (event) => {
    const after = event.data.after.val();
    if (!after || !after.action) {
      return;
    }

    const deviceId = event.params.deviceId;
    const db = getDatabase();
    const nameSnap = await db.ref(`devices/${deviceId}/status/name`).once("value");
    const name = nameSnap.val() || deviceId;

    const isStart = (after.action === "start");
    const title = isStart
      ? `\u26a0\ufe0f ${name}: Failed to Start`
      : `\u26a0\ufe0f ${name}: Failed to Stop`;
    const message = isStart
      ? `\u26a0\ufe0f Alert: ${name} failed to start. Please check the starter panel.`
      : `\u26a0\ufe0f Alert: ${name} did not stop. Motor is still running! Please check the starter panel.`;

    try {
      await sendAlertToDevice({ deviceId, name, title, body: message, whatsappMessage: message });
      logger.info(`[CommandFailedAlert] ${deviceId} (${name}): ${after.action} failure`);
    } catch (err) {
      logger.error(`[CommandFailedAlert] ${deviceId} (${name}): send failed: ${err}`);
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
  if (!token || typeof token !== "string" || token.length > 500) {
    throw new HttpsError("invalid-argument", "Missing or invalid FCM token.");
  }

  const rawTitle = (request.data && request.data.title) || "Furrow test";
  const rawBody = (request.data && request.data.body) ||
    "If you see this, push notifications work.";
  const title = String(rawTitle).slice(0, 100);
  const body = String(rawBody).slice(0, 500);

  try {
    const messageId = await getMessaging().send({
      token,
      // data, not notification - see sendPushToDevice()'s comment for
      // why (avoids a duplicate auto-displayed notification alongside
      // our own handler's).
      data: {
        title,
        body,
        url: "./dashboard.html",
        click_action: "./dashboard.html",
      },
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
    res.set("Cache-Control", "public, max-age=86400, s-maxage=86400");
    Readable.fromWeb(upstream.body).pipe(res);
  } catch (err) {
    logger.error(`[FirmwareProxy] fetch failed for ${tag}/${file}: ${err}`);
    res.status(502).send("Failed to fetch firmware from GitHub");
  }
});
