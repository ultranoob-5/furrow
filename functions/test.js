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
assert(typeof mod.isValidFirmwareAssetRequest === "function", "index.js failed to export isValidFirmwareAssetRequest as a function");
assert(typeof mod.autoResumeWatchdog === "function", "index.js failed to export autoResumeWatchdog as a function");
assert(typeof mod.runAutoResumeWatchdog === "function", "index.js failed to export runAutoResumeWatchdog as a function");
assert(typeof mod.shouldAutoResume === "function", "index.js failed to export shouldAutoResume as a function");
assert(typeof mod.clampAutoResumeDelayMinutes === "function", "index.js failed to export clampAutoResumeDelayMinutes as a function");
console.log("Module load check: PASS (this is the exact check that would have caught the admin.database() bug)\n");

const { runWatchdog, runAutoResumeWatchdog, isValidFirmwareAssetRequest } = mod;

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

// Safety-relevant: this decides whether a physical motor auto-starts
// with no human confirming in the moment. Tested directly rather than
// trusted from reading the code - silence (missing settings, wrong
// type, anything not exactly the expected shape) must always resolve
// to "don't resume," never accidentally to "do."
function testShouldAutoResume() {
  const cases = [
    // [autoResumeSettings, motorStateBeforeOutage, expected, description]
    [{ enabled: true }, "RUNNING", true, "enabled + was running -> resumes"],
    [{ enabled: true }, "OFF", false, "enabled + was off -> does not resume"],
    [{ enabled: true }, undefined, false, "enabled + no snapshot at all -> does not resume"],
    [{ enabled: true }, null, false, "enabled + null snapshot -> does not resume"],
    [{ enabled: false }, "RUNNING", false, "disabled + was running -> does not resume"],
    [null, "RUNNING", false, "no settings object at all -> does not resume"],
    [undefined, "RUNNING", false, "undefined settings -> does not resume"],
    [{}, "RUNNING", false, "settings object with no enabled field -> does not resume"],
    [{ enabled: "true" }, "RUNNING", false, "enabled as a string, not boolean true -> does not resume"],
    [{ enabled: 1 }, "RUNNING", false, "enabled as a truthy number, not boolean true -> does not resume"],
  ];

  let failures = 0;
  for (const [settings, stateBefore, expected, description] of cases) {
    const actual = mod.shouldAutoResume(settings, stateBefore);
    const pass = actual === expected;
    console.log(`[${pass ? "PASS" : "FAIL"}] ${description} -> ${actual}`);
    if (!pass) failures++;
  }

  assert(failures === 0, `${failures} shouldAutoResume case(s) failed - see above`);
  console.log("\nshouldAutoResume: ALL 10 CASES PASS\n");
}

function testClampAutoResumeDelayMinutes() {
  const cases = [
    [5, 5, "within range, unchanged"],
    [1, 1, "lower bound, unchanged"],
    [10, 10, "upper bound, unchanged"],
    [0, 1, "below range, clamped up to 1"],
    [15, 10, "above range, clamped down to 10"],
    [-5, 1, "negative, clamped up to 1"],
    [undefined, 1, "undefined, falls back to 1"],
    [null, 1, "null, falls back to 1"],
    ["abc", 1, "non-numeric string, falls back to 1"],
    ["7", 7, "numeric string, coerced correctly"],
  ];

  let failures = 0;
  for (const [input, expected, description] of cases) {
    const actual = mod.clampAutoResumeDelayMinutes(input);
    const pass = actual === expected;
    console.log(`[${pass ? "PASS" : "FAIL"}] ${description}: clampAutoResumeDelayMinutes(${JSON.stringify(input)}) -> ${actual}`);
    if (!pass) failures++;
  }

  assert(failures === 0, `${failures} clampAutoResumeDelayMinutes case(s) failed - see above`);
  console.log("\nclampAutoResumeDelayMinutes: ALL 10 CASES PASS\n");
}

testShouldAutoResume();
testClampAutoResumeDelayMinutes();

async function testRunAutoResumeWatchdog() {
  const nowMs = Date.now();

  const fakeDevices = {
    "dev-due":        { autoResume: { enabled: true, dueAt: nowMs - 1000 } },  // 1s in the past - due
    "dev-not-yet":    { autoResume: { enabled: true, dueAt: nowMs + 60000 } }, // 1min in the future - not due yet
    "dev-nothing":    { status: { name: "Nothing scheduled" } },               // no autoResume field at all
    "dev-send-fails": { autoResume: { enabled: true, dueAt: nowMs - 1000 } },  // due, but the send will throw
  };

  const started = [];
  const cleared = [];

  const resumed = await runAutoResumeWatchdog({
    fetchDevices: async () => fakeDevices,
    sendStartCommand: async (deviceId) => {
      if (deviceId === "dev-send-fails") {
        throw new Error("simulated send failure");
      }
      started.push(deviceId);
    },
    clearDueAt: async (deviceId) => {
      cleared.push(deviceId);
    },
    nowMs,
  });

  console.log("=== runAutoResumeWatchdog results ===");
  console.log("Resumed:", resumed);
  console.log("Start commands sent:", started);
  console.log("dueAt cleared:", cleared);

  assert(resumed.length === 1 && resumed[0] === "dev-due", `Expected only dev-due to be resumed, got ${JSON.stringify(resumed)}`);
  assert(started.includes("dev-due"), "Expected a start command sent for dev-due");
  assert(!started.includes("dev-not-yet"), "Expected no start command for dev-not-yet (delay hasn't elapsed)");
  assert(!started.includes("dev-nothing"), "Expected no start command for dev-nothing (nothing scheduled)");
  assert(cleared.includes("dev-due"), "Expected dueAt cleared for dev-due after a successful send");
  assert(!cleared.includes("dev-send-fails"), "Expected dueAt NOT cleared for dev-send-fails - a failed send should retry next run, not be silently dropped");

  console.log("\nrunAutoResumeWatchdog: ALL ASSERTIONS PASS\n");
}

async function main() {
  const nowMs = Date.now();

  const fakeDevices = {
    "dev-fresh":     { status: { lastSeen: nowMs - 5000,  name: "Fresh",     whatsappPhone: "111", powerAlertSent: false } },
    "dev-stale-new": { status: { lastSeen: nowMs - 45000, name: "StaleNew",  whatsappPhone: "222", powerAlertSent: false }, motor: { state: "RUNNING" } },
    "dev-stale-old": { status: { lastSeen: nowMs - 90000, name: "StaleOld",  whatsappPhone: "333", powerAlertSent: true } },
    "dev-no-phone":  { status: { lastSeen: nowMs - 60000, name: "NoPhone",   powerAlertSent: false } },
    "dev-push-only": { status: { lastSeen: nowMs - 70000, name: "PushOnly",  powerAlertSent: false } },
    "dev-never":     { status: { name: "Never" } },
  };

  const sent = [];
  const pushSent = [];
  const flagsSet = [];
  const motorStatesSnapshotted = {};

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

  assert(motorStatesSnapshotted["dev-stale-new"] === "RUNNING",
    `Expected dev-stale-new's motor state (RUNNING) to be snapshotted at outage detection, got ${motorStatesSnapshotted["dev-stale-new"]}`);
  assert(motorStatesSnapshotted["dev-push-only"] === undefined,
    `Expected dev-push-only (no motor field at all in the fake) to snapshot as undefined, got ${motorStatesSnapshotted["dev-push-only"]}`);

  console.log("\nALL ASSERTIONS PASS:");
  console.log("- dev-fresh (5s): correctly skipped, too recent");
  console.log("- dev-stale-new (45s, WhatsApp + push configured): correctly alerted via both, flagged");
  console.log("- dev-stale-old (90s, already alerted): correctly skipped, dedup working");
  console.log("- dev-no-phone (60s, no WhatsApp AND no push tokens): correctly skipped, no crash");
  console.log("- dev-push-only (70s, push only, no WhatsApp phone): correctly alerted via push alone - the whole point of decoupling the two channels");
  console.log("- dev-never (no lastSeen ever): correctly skipped, no crash");
}

main()
  .then(testRunAutoResumeWatchdog)
  .catch((err) => {
    console.error("Test run failed:", err);
    process.exit(1);
  });
