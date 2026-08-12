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
console.log("Module load check: PASS (this is the exact check that would have caught the admin.database() bug)\n");

const { runWatchdog } = mod;

async function main() {
  const nowMs = Date.now();

  const fakeDevices = {
    "dev-fresh":     { status: { lastSeen: nowMs - 5000,  name: "Fresh",    whatsappPhone: "111", powerAlertSent: false } },
    "dev-stale-new": { status: { lastSeen: nowMs - 45000, name: "StaleNew", whatsappPhone: "222", powerAlertSent: false } },
    "dev-stale-old": { status: { lastSeen: nowMs - 90000, name: "StaleOld", whatsappPhone: "333", powerAlertSent: true } },
    "dev-no-phone":  { status: { lastSeen: nowMs - 60000, name: "NoPhone",  powerAlertSent: false } },
    "dev-never":     { status: { name: "Never" } },
  };

  const sent = [];
  const flagsSet = [];

  const alerted = await runWatchdog({
    fetchDevices: async () => fakeDevices,
    sendWhatsApp: async (phone, message) => {
      sent.push([phone, message]);
    },
    setDedupFlag: async (deviceId) => {
      flagsSet.push(deviceId);
    },
    nowMs,
  });

  console.log("=== Results ===");
  console.log("Alerted:", alerted);
  console.log("Sent:", sent);
  console.log("Flags set:", flagsSet);

  assert(alerted.length === 1, `Expected exactly 1 alert, got ${alerted.length}`);
  assert(alerted[0] === "dev-stale-new", `Expected dev-stale-new to be alerted, got ${alerted[0]}`);
  assert(sent.length === 1 && sent[0][0] === "222", "Expected phone 222 to receive the alert");
  assert(sent[0][1].includes("StaleNew"), "Expected message to mention StaleNew");
  assert(flagsSet.length === 1 && flagsSet[0] === "dev-stale-new", "Expected dedup flag set for dev-stale-new");

  console.log("\nALL ASSERTIONS PASS:");
  console.log("- dev-fresh (5s): correctly skipped, too recent");
  console.log("- dev-stale-new (45s, not yet alerted): correctly alerted + flagged");
  console.log("- dev-stale-old (90s, already alerted): correctly skipped, dedup working");
  console.log("- dev-no-phone (60s, no phone): correctly skipped, no crash");
  console.log("- dev-never (no lastSeen ever): correctly skipped, no crash");
}

main().catch((err) => {
  console.error("Test run failed:", err);
  process.exit(1);
});
