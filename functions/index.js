const { onSchedule } = require("firebase-functions/v2/scheduler");
const { defineSecret } = require("firebase-functions/params");
const { logger } = require("firebase-functions");
const admin = require("firebase-admin");
const { getDatabase } = require("firebase-admin/database");

admin.initializeApp();

const whapiToken = defineSecret("WHAPI_TOKEN");

const OFFLINE_THRESHOLD_MS = 30000;

// Pure decision + action logic, separated from the scheduled trigger so
// it can be unit tested with fake fetchDevices/sendWhatsApp/setDedupFlag
// functions instead of needing the Firebase emulator - see test.js.
async function runWatchdog({ fetchDevices, sendWhatsApp, setDedupFlag, nowMs }) {
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

    const ageMs = nowMs - lastSeen;

    if (ageMs < OFFLINE_THRESHOLD_MS) {
      continue; // still within normal heartbeat range
    }

    if (alreadyAlerted) {
      continue; // already sent for this outage, don't spam every run
    }

    if (!phone) {
      logger.info(`${deviceId} (${name}): offline ${Math.floor(ageMs / 1000)}s, no WhatsApp recipient configured - skipping`);
      continue;
    }

    logger.info(`${deviceId} (${name}): offline ${Math.floor(ageMs / 1000)}s - alerting ${phone}`);

    try {
      await sendWhatsApp(phone, `\u26a0\ufe0f ${name} appears to have lost power - no contact for over 30s`);
    } catch (err) {
      logger.error(`  WhatsApp send failed: ${err}`);
      continue; // don't mark alerted if the send itself failed
    }

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
      setDedupFlag: async (deviceId) => {
        await db.ref(`devices/${deviceId}/status/powerAlertSent`).set(true);
      },
      nowMs: Date.now(),
    });
  }
);

module.exports.runWatchdog = runWatchdog; // exported for test.js
