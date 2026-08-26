// Standalone test for runWatchdog()'s decision logic - no Firebase
// emulator needed, just fake implementations of the three injected
// functions. Run with: node test.js
//
// Mirrors the exact 5 scenarios the original GitHub Actions Python
// version (.github/scripts/power_watchdog.py) was tested against,
// since this is a port of the same logic to Cloud Functions.
//
// Also does a plain module-load check first - a real "admin.database
// is not a function" bug (firebase-admin v14 removed the namespaced
// API in favor of getDatabase() from firebase-admin/database) shipped
// to production once already because the decision-logic tests alone,
// with fetchDevices mocked out, never actually exercised the real
// require()/getDatabase() call at module load time.

function assert(condition, message) {
  if (!condition) {
    console.error("FAIL:", message);
    process.exit(1);
  }
}

const mod = require("./index.js");
assert(typeof mod.powerWatchdog === "function", "index.js failed to export powerWatchdog as a function - check for a module-load-time error above");
assert(typeof mod.runWatchdog === "function", "index.js failed to export runWatchdog as a function");
assert(typeof mod.onMotorStateChanged === "function", "index.js failed to export onMotorStateChanged as a function");
assert(typeof mod.onPowerRestored === "function", "index.js failed to export onPowerRestored as a function");
assert(typeof mod.onMotorCommandFailed === "function", "index.js failed to export onMotorCommandFailed as a function");
assert(typeof mod.sendTestNotification === "function", "index.js failed to export sendTestNotification as a function");
console.log("Module load check: PASS (this is the exact check that would have caught the admin.database() bug)\n");

const { runWatchdog } = mod;

async function main() {
  const nowMs = Date.now();

  const fakeDevices = {
    "dev-fresh":     { status: { lastSeen: nowMs - 5000,  name: "Fresh",     whatsappPhone: "111", powerAlertSent: false } },
    "dev-stale-new": { status: { lastSeen: nowMs - 45000, name: "StaleNew",  whatsappPhone: "222", powerAlertSent: false } },
    "dev-stale-old": { status: { lastSeen: nowMs - 90000, name: "StaleOld",  whatsappPhone: "333", powerAlertSent: true } },
    "dev-no-phone":  { status: { lastSeen: nowMs - 60000, name: "NoPhone",   powerAlertSent: false } },
    "dev-push-only": { status: { lastSeen: nowMs - 70000, name: "PushOnly",  powerAlertSent: false } },
    "dev-never":     { status: { name: "Never" } },
  };

  const sent = [];
  const pushSent = [];
  const flagsSet = [];

  // Devices that "have push tokens registered" in this fake - everyone
  // except dev-no-phone (whose whole point is testing "neither channel
  // configured" still gets skipped safely) and the ones that shouldn't
  // reach the send stage at all (dev-fresh, dev-stale-old, dev-never).
  const hasPushTokens = new Set(["dev-stale-new", "dev-push-only"]);

  const alerted = await runWatchdog({
    fetchDevices: async () => fakeDevices,
    sendWhatsApp: async (phone, message) => {
      sent.push([phone, message]);
    },
    sendPush: async (deviceId, title, body) => {
      if (!hasPushTokens.has(deviceId)) {
        return { sent: 0, pruned: 0 }; // simulates no tokens registered
      }
      pushSent.push([deviceId, title, body]);
      return { sent: 1, pruned: 0 };
    },
    setDedupFlag: async (deviceId) => {
      flagsSet.push(deviceId);
    },
    nowMs,
  });

  console.log("=== Results ===");
  console.log("Alerted:", alerted);
  console.log("WhatsApp sent:", sent);
  console.log("Push sent:", pushSent);
  console.log("Flags set:", flagsSet);

  assert(alerted.length === 2, `Expected exactly 2 alerts, got ${alerted.length}`);
  assert(alerted.includes("dev-stale-new"), "Expected dev-stale-new to be alerted (has both WhatsApp and push)");
  assert(alerted.includes("dev-push-only"), "Expected dev-push-only to be alerted (push alone, no WhatsApp phone configured)");
  assert(!alerted.includes("dev-no-phone"), "Expected dev-no-phone to NOT be alerted (neither channel configured)");

  assert(sent.length === 1 && sent[0][0] === "222", "Expected phone 222 (dev-stale-new) to be the only WhatsApp recipient");
  assert(sent[0][1].includes("StaleNew"), "Expected WhatsApp message to mention StaleNew");

  assert(pushSent.length === 2, `Expected exactly 2 push sends, got ${pushSent.length}`);
  assert(pushSent.some(([id]) => id === "dev-stale-new"), "Expected a push send for dev-stale-new");
  assert(pushSent.some(([id]) => id === "dev-push-only"), "Expected a push send for dev-push-only");

  assert(flagsSet.length === 2 && flagsSet.includes("dev-stale-new") && flagsSet.includes("dev-push-only"),
    "Expected dedup flags set for both alerted devices");

  console.log("\nALL ASSERTIONS PASS:");
  console.log("- dev-fresh (5s): correctly skipped, too recent");
  console.log("- dev-stale-new (45s, WhatsApp + push configured): correctly alerted via both, flagged");
  console.log("- dev-stale-old (90s, already alerted): correctly skipped, dedup working");
  console.log("- dev-no-phone (60s, no WhatsApp AND no push tokens): correctly skipped, no crash");
  console.log("- dev-push-only (70s, push only, no WhatsApp phone): correctly alerted via push alone - the whole point of decoupling the two channels");
  console.log("- dev-never (no lastSeen ever): correctly skipped, no crash");
}

main().catch((err) => {
  console.error("Test run failed:", err);
  process.exit(1);
});
