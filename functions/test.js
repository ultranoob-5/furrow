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
assert(typeof mod.firmwareProxy === "function", "index.js failed to export firmwareProxy as a function");
assert(mod.scheduleWatchdog === undefined, "scheduleWatchdog should no longer be exported (schedule migrated to firmware)");
console.log("Module load check: PASS (this is the exact check that would have caught the admin.database() bug)\n");

const { runWatchdog, isValidFirmwareAssetRequest } = mod;

// Security-relevant: firmwareProxy builds an outbound GitHub URL from
// these two query params, so a validation bug here is a real open-
// proxy/SSRF risk, not just a correctness nitpick. Tested directly
// rather than assumed correct from reading the regex/allowlist.
function testFirmwareAssetValidation() {
  const cases = [
    // [tag, file, expectedValid, description]
    ["v1.3.4", "firmware.bin", true, "valid tag + valid file"],
    ["v1.3.4", "bootloader.bin", true, "valid tag + bootloader.bin"],
    ["v1.3.4", "partitions.bin", true, "valid tag + partitions.bin"],
    ["v1.3.4", "boot_app0.bin", true, "valid tag + boot_app0.bin"],
    ["v0.1.1", "firmware.bin", true, "valid older tag still accepted"],
    ["v1.3.4", "readme.md", false, "file not in the allowlist"],
    ["v1.3.4", "../../../etc/passwd", false, "path traversal in file"],
    ["v1.3.4", "firmware.bin?x=1", false, "trailing junk on an otherwise-valid file"],
    ["1.3.4", "firmware.bin", false, "tag missing the leading v"],
    ["v1.3", "firmware.bin", false, "tag missing the patch component"],
    ["v1.3.4.5", "firmware.bin", false, "tag with an extra component"],
    ["v1.3.4-beta", "firmware.bin", false, "tag with a suffix"],
    ["main", "firmware.bin", false, "a branch name instead of a tag"],
    ["v1.3.4; rm -rf /", "firmware.bin", false, "shell-injection-shaped tag"],
    ["https://evil.example.com/", "firmware.bin", false, "a full URL as the tag"],
    [null, "firmware.bin", false, "null tag"],
    [undefined, "firmware.bin", false, "undefined tag"],
    ["v1.3.4", null, false, "null file"],
    ["v1.3.4", undefined, false, "undefined file"],
    [123, "firmware.bin", false, "numeric tag (not a string)"],
    ["v1.3.4", "", false, "empty string file"],
    ["", "firmware.bin", false, "empty string tag"],
  ];

  let failures = 0;
  for (const [tag, file, expected, description] of cases) {
    const actual = isValidFirmwareAssetRequest(tag, file);
    const pass = actual === expected;
    console.log(`[${pass ? "PASS" : "FAIL"}] ${description} (tag=${JSON.stringify(tag)}, file=${JSON.stringify(file)}) -> ${actual}`);
    if (!pass) failures++;
  }

  assert(failures === 0, `${failures} firmwareProxy validation case(s) failed - see above`);
  console.log("\nfirmwareProxy validation: ALL 21 CASES PASS\n");
}

testFirmwareAssetValidation();



async function main() {
  const nowMs = Date.now();

  const fakeDevices = {
    "dev-fresh":     { status: { lastSeen: nowMs - 5000,  name: "Fresh",     whatsappPhone: "111", powerAlertSent: false } },
    "dev-stale-new": { status: { lastSeen: nowMs - 45000, name: "StaleNew",  whatsappPhone: "222", powerAlertSent: false }, motor: { state: "RUNNING" } },
    "dev-renamed":       { displayName: "Well Pump", status: { lastSeen: nowMs - 45000, name: "Farm Pump", whatsappPhone: "555", powerAlertSent: false }, motor: { state: "RUNNING" } },
    "dev-cleared-phone": { whatsappPhone: "", status: { lastSeen: nowMs - 45000, name: "ClearedPhone", whatsappPhone: "999", powerAlertSent: false }, motor: { state: "RUNNING" } },
    "dev-stale-old":     { status: { lastSeen: nowMs - 90000, name: "StaleOld",  whatsappPhone: "333", powerAlertSent: true } },
    "dev-stale-off":     { status: { lastSeen: nowMs - 55000, name: "StaleOff",  whatsappPhone: "444", powerAlertSent: false }, motor: { state: "OFF" } },
    "dev-no-phone":      { status: { lastSeen: nowMs - 60000, name: "NoPhone",   powerAlertSent: false }, motor: { state: "RUNNING" } },
    "dev-push-only":     { status: { lastSeen: nowMs - 70000, name: "PushOnly",  powerAlertSent: false }, motor: { state: "RUNNING" } },
    "dev-never":         { status: { name: "Never" } },
  };

  const sent = [];
  const pushSent = [];
  const flagsSet = [];
  const motorStatesSnapshotted = {};

  // Devices that "have push tokens registered" in this fake - everyone
  // except dev-no-phone (whose whole point is testing "neither channel
  // configured" still gets skipped safely) and the ones that shouldn't
  // reach the send stage at all (dev-fresh, dev-stale-old, dev-stale-off, dev-never).
  const hasPushTokens = new Set(["dev-stale-new", "dev-renamed", "dev-cleared-phone", "dev-push-only", "dev-stale-off"]);

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
    setDedupFlag: async (deviceId, motorStateAtOutage) => {
      flagsSet.push(deviceId);
      motorStatesSnapshotted[deviceId] = motorStateAtOutage;
    },
    nowMs,
  });

  console.log("=== Results ===");
  console.log("Alerted:", alerted);
  console.log("WhatsApp sent:", sent);
  console.log("Push sent:", pushSent);
  console.log("Flags set:", flagsSet);

  assert(alerted.length === 4, `Expected exactly 4 alerts, got ${alerted.length}`);
  assert(alerted.includes("dev-stale-new"), "Expected dev-stale-new to be alerted (has both WhatsApp and push)");
  assert(alerted.includes("dev-renamed"), "Expected dev-renamed to be alerted (has both WhatsApp and push)");
  assert(alerted.includes("dev-cleared-phone"), "Expected dev-cleared-phone to be alerted (push only, WhatsApp was cleared)");
  assert(alerted.includes("dev-push-only"), "Expected dev-push-only to be alerted (push alone, no WhatsApp phone configured)");
  assert(!alerted.includes("dev-no-phone"), "Expected dev-no-phone to NOT be alerted (neither channel configured)");
  assert(!alerted.includes("dev-stale-off"), "Expected dev-stale-off to NOT be alerted (motor was OFF at outage)");

  assert(sent.length === 2, `Expected exactly 2 WhatsApp messages, got ${sent.length}`);
  const staleNewWa = sent.find(([phone]) => phone === "222");
  assert(staleNewWa && staleNewWa[1].includes("StaleNew"), "Expected WhatsApp message to mention StaleNew for phone 222");

  const renamedWa = sent.find(([phone]) => phone === "555");
  assert(renamedWa, "Expected WhatsApp message to phone 555 for dev-renamed");
  assert(renamedWa[1].includes("Well Pump"), `Expected WhatsApp message to use displayName 'Well Pump', got: ${renamedWa[1]}`);
  assert(!renamedWa[1].includes("Farm Pump"), `Expected WhatsApp message to NOT contain old status.name 'Farm Pump', got: ${renamedWa[1]}`);

  // dev-cleared-phone must NOT have sent to phone "999"
  assert(!sent.some(([phone]) => phone === "999"), "Expected phone 999 to NOT receive WhatsApp message because whatsappPhone was cleared");

  assert(pushSent.length === 4, `Expected exactly 4 push sends, got ${pushSent.length}`);
  assert(pushSent.some(([id]) => id === "dev-stale-new"), "Expected a push send for dev-stale-new");
  assert(pushSent.some(([id]) => id === "dev-renamed"), "Expected a push send for dev-renamed");
  assert(pushSent.some(([id]) => id === "dev-cleared-phone"), "Expected a push send for dev-cleared-phone");
  assert(pushSent.some(([id]) => id === "dev-push-only"), "Expected a push send for dev-push-only");

  const renamedPush = pushSent.find(([id]) => id === "dev-renamed");
  assert(renamedPush[1].includes("Well Pump") && !renamedPush[1].includes("Farm Pump"),
    `Expected push notification title to use displayName 'Well Pump', got: ${renamedPush[1]}`);
  assert(renamedPush[2].includes("Well Pump") && !renamedPush[2].includes("Farm Pump"),
    `Expected push notification body to use displayName 'Well Pump', got: ${renamedPush[2]}`);

  assert(flagsSet.length === 4 && flagsSet.includes("dev-stale-new") && flagsSet.includes("dev-renamed") && flagsSet.includes("dev-cleared-phone") && flagsSet.includes("dev-push-only"),
    "Expected dedup flags set for all alerted devices");

  assert(motorStatesSnapshotted["dev-stale-new"] === "RUNNING",
    `Expected dev-stale-new's motor state (RUNNING) to be snapshotted at outage detection, got ${motorStatesSnapshotted["dev-stale-new"]}`);
  assert(motorStatesSnapshotted["dev-renamed"] === "RUNNING",
    `Expected dev-renamed's motor state (RUNNING) to be snapshotted at outage detection, got ${motorStatesSnapshotted["dev-renamed"]}`);
  assert(motorStatesSnapshotted["dev-cleared-phone"] === "RUNNING",
    `Expected dev-cleared-phone's motor state (RUNNING) to be snapshotted at outage detection, got ${motorStatesSnapshotted["dev-cleared-phone"]}`);
  assert(motorStatesSnapshotted["dev-push-only"] === "RUNNING",
    `Expected dev-push-only's motor state (RUNNING) to be snapshotted at outage detection, got ${motorStatesSnapshotted["dev-push-only"]}`);

  console.log("\nALL ASSERTIONS PASS:");
  console.log("- dev-fresh (5s): correctly skipped, too recent");
  console.log("- dev-stale-new (45s, motor RUNNING, WhatsApp + push configured): correctly alerted via both, flagged");
  console.log("- dev-renamed (45s, motor RUNNING, displayName 'Well Pump' overrides status.name 'Farm Pump'): correctly alerted using displayName");
  console.log("- dev-cleared-phone (45s, motor RUNNING, whatsappPhone: ''): correctly skipped WhatsApp and alerted via Push only");
  console.log("- dev-stale-old (90s, already alerted): correctly skipped, dedup working");
  console.log("- dev-stale-off (55s, motor OFF): correctly skipped without alert even with phone and push registered");
  console.log("- dev-no-phone (60s, motor RUNNING, no WhatsApp AND no push tokens): correctly skipped, no crash");
  console.log("- dev-push-only (70s, motor RUNNING, push only, no WhatsApp phone): correctly alerted via push alone - the whole point of decoupling the two channels");
  console.log("- dev-never (no lastSeen ever): correctly skipped, no crash");
}

main().catch((err) => {
  console.error("Test run failed:", err);
  process.exit(1);
});
