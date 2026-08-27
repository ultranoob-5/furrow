
## [1.3.5] - 2026-08-26

No firmware code changes - identical firmware.bin to v1.3.4. Re-tagged
purely to get a release published through the updated
build-and-release.yml (added after v1.3.4 was tagged), which now
publishes the full flash set (bootloader.bin, partitions.bin,
boot_app0.bin alongside firmware.bin) that web/flash.html's browser
flasher needs for a first-time flash of a blank chip.

v1.3.2/v1.3.3/v1.3.4's own releases will never pick this up no matter
how many times their workflow runs are re-triggered - GitHub Actions
executes a tag-triggered workflow exactly as that workflow file
existed at the tagged commit, not whatever's currently on main. Those
three tags all predate the build-and-release.yml change, so their
release workflow runs, however many times re-run, only ever have the
old firmware.bin-only version of the file available to execute. Only
a new tag pointing at a commit on or after that change actually picks
up the new behavior - this is that tag.

## [1.3.4] - 2026-08-26

Found via detailed external review, not this project's own testing -
verified each claim against the actual code (via simulation, since
this sandbox has no ESP32 hardware) before acting on it.

- **The real bug**: CurrentSensor::update() takes at most one ADC
  sample per call, and its 1ms rate-limiting (SAMPLE_INTERVAL_US) only
  does anything if update() is called faster than that. It's called
  once per main.cpp loop() iteration, which - dominated by the
  previous delay(10) plus everything else loop() does - was cycling
  roughly every ~12ms, not ~1ms. Confirmed by simulating the exact
  scheduling logic: a "400 samples @ 1ms = 400ms" window was actually
  taking ~4.8 real seconds (12x slower), and RUN_CONFIRM_WINDOWS's
  documented "~1.2s to confirm a start" was really more like 14+
  seconds. This had been sitting underneath essentially all of the
  earlier CT threshold/debounce tuning work without ever being
  identified as the actual cause of anything.
- Fix: reduced loop()'s delay(10) to delay(1) - still yields to
  FreeRTOS (unlike removing it entirely, which risks starving the
  WiFi stack or the watchdog), while letting loop() cycle often enough
  for CurrentSensor's existing 1ms rate-limit to actually become the
  binding constraint again. Doesn't guarantee hitting the intended
  rate exactly - cloud.loop() especially can still occasionally take
  longer than 1ms during real network I/O - so added an unconditional
  per-window timing log (CurrentSensor, tag "Current": "Window: Xms
  (intended 400ms)") so what's actually being achieved on real
  hardware is directly observable, not assumed. This is the one part
  of this release that can only really be verified by watching Serial
  output on the real device - recommend checking that log after
  flashing.
- Updated the SAMPLES and RUN_CONFIRM_WINDOWS comments in
  current_sensor.h to stop stating a fixed timing guarantee that
  wasn't actually being met, pointing to the new log instead.

- **Related but separate bug**: setup() published motor state to
  Firebase (cloud.publishMotor()) before the first real CT reading
  existed, meaning a motor that's actually running across a reboot
  (most plausible after a deliberate remote restart or firmware
  update) showed "OFF" in Firebase until the first real reading
  corrected it - a window whose real duration was however long the
  bug above was making it, up to several seconds or more. The
  baselineEstablished fix in v1.3.3 only stopped this from being
  misclassified as a "manual start" once corrected; it didn't stop
  the wrong initial value from being published at all. Fixed by
  removing publishMotor() from setup() entirely and gating every
  other call site (Cloud::loop()'s heartbeat, handlePendingCommand()'s
  post-command republish) behind motor.hasReading() - nothing
  publishes a motor-state guess before a real reading exists anywhere
  in the firmware now. The first real state publishes immediately
  once known (from loop()'s baseline-establishment point), not
  whenever the next heartbeat happens to land.

Also reviewed and deliberately left unchanged, since both the
external review and this project's own prior notes independently
concluded the same thing:
- OFF detection staying single-window/undebounced (current_sensor.h) -
  a real tradeoff already made deliberately for fast stop reporting,
  worth revisiting only if real motor behavior later shows occasional
  false OFF readings.
- Motor::start()'s "already RUNNING per CT, skip" guard - the
  asymmetric counterpart to the STOP guard removed in v1.3.3, flagged
  there as deliberately deferred until CT timing reliability (the
  actual subject of this release) was solid enough to trust.
- The R6/ICAL calibration gap (current_sensor.h) - already documented,
  doesn't affect the ON/OFF threshold logic (empirically calibrated
  against real hardware behavior, not the theoretical ICAL value), so
  not worth resolving unless real energy-monitoring accuracy is
  needed later.

## [1.3.3] - 2026-08-26

Fixes found during a full line-by-line review of the entire codebase,
not from a specific bug report:

- Fixed pressStopButton() having the same "already mid-pulse, ignore"
  guard as pressStartButton() - a STOP arriving while a START pulse
  was still active (well within 500ms, entirely plausible: hit Start,
  immediately correct with Stop) did nothing at all. This directly
  undermined the v1.3.2 Motor::stop() fix - that fix only guaranteed
  Motor::stop() calls pressStopButton(), not that pressStopButton()
  actually acts on it. STOP now always preempts an in-progress START.
- JsonUtil::escape() only escaped 4 of the ~34 characters JSON
  actually requires escaping (RFC 8259 mandates all of U+0000-U+001F,
  not just the common ones). A pasted control character in a device
  name or phone number would have produced invalid JSON, silently
  defeating the exact protection this function exists to provide.
- provisioning.cpp's WiFi scan JSON never escaped SSIDs at all, and
  didn't even include json_util.h. A nearby network's SSID (entirely
  outside your control) containing a '"' would have broken the setup
  page's network dropdown.
- Fixed a false "started manually at the panel" push notification
  possible when the device reboots while the motor is already running
  (most plausible after a deliberate remote restart or firmware
  update) - previousMotorRunning was seeded from CurrentSensor's
  default (not-running) state before any real reading had completed,
  making the first real reading look like a fresh transition. Now
  waits for Motor::hasReading() before establishing the baseline.
- Network::begin() now retries the stored WiFi credentials up to 3
  times (each with the existing 15s timeout) before falling back to
  full reprovisioning, instead of giving up after one attempt. A
  transient issue right at boot - the router still rebooting after a
  shared power blip, for instance - no longer wipes perfectly good
  credentials and strands the device waiting for someone to physically
  visit and reconfigure it.

Also fixed on the dashboard side (web/, no firmware impact):
- Logging out never called devicesRef.off() - the original Firebase
  listener from startDeviceListener() is persistent and never
  auto-cleans up, so repeated login/logout cycles in one browser
  session accumulated multiple simultaneous listeners, each firing
  redundantly on every future device update.
- The factory-reset confirmation trimmed the typed input but never
  the stored device name it's compared against - a name with an
  accidental leading/trailing space (easy to type by mistake) made
  the confirmation permanently unsatisfiable, since the person can't
  see or reasonably type an invisible space to match.

## [1.3.2] - 2026-08-26

- Fixed a real bug in Motor::stop(): an early-return guard
  (`if (state == MotorState::OFF) return;`) could silently discard a
  genuine STOP command whenever the CT-sensed state happened to read
  OFF at that moment - including a false reading, since the OFF-
  detection path is deliberately immediate/undebounced (unlike
  RUNNING, which requires 3 consecutive confirming windows) and a
  single noisy RMS window is enough to trigger it. The comment right
  above this code already stated the correct intended behavior ("the
  physical starter remains the authority for stopping the motor") -
  the guard clause just didn't actually implement what the comment
  described. Removed it: STOP is now always sent unconditionally,
  regardless of what the last known state was. Pulsing an
  already-stopped motor's stop button is a harmless no-op, the same
  as a person pressing Stop twice.
- Motor::start() has an analogous guard (skips re-sending START if
  state already reads RUNNING) with the same theoretical risk, though
  lower in practice since RUNNING requires 3 confirming windows before
  it's ever reported - left as-is for now, flagged for a separate
  decision rather than changed unprompted alongside an unrelated
  report.

## [1.3.1] - 2026-08-26

- Motor-started push notifications now say whether it was a remote
  command or someone at the panel: "Motor started via remote command"
  vs "Motor started manually at the panel", instead of just "Motor
  started." Detected by exclusion, not by directly sensing the
  physical button (which the ESP32 has no way to do): if a real start
  is detected and no remote start command is currently awaiting
  confirmation, nothing else could have caused it.
- Deliberately start-only, not stop: a motor stopping without a
  remote command in flight could mean several different things (power
  loss, an overload trip, a genuine manual stop), so guessing "manual"
  there would often be wrong. A motor starting only has two possible
  causes, so the same inference is safe there and not for stops.
- New optional startedVia field in the motor RTDB node ("remote" |
  "manual"), only ever present on the specific publish that caught a
  transition into RUNNING - never on the routine heartbeat republish
  or an OFF transition, and automatically cleared by the next publish
  that omits it (database.set()'s full-replace semantics).

## [1.3.0] - 2026-08-26

- Added failed start/stop detection and alerting: a remote Start/Stop
  command that doesn't actually take effect within 30 seconds (checked
  against real CT current feedback, not just whether the relay pulse
  was sent) now fires an alert on both WhatsApp and push - a stuck
  contactor, tripped overload relay, or blown fuse no longer fails
  silently. Remote commands only - a physical button press at the
  panel never routes through the device's own command handling, so
  there's nothing to compare against for that case.
- New Cloud Function, onMotorCommandFailed, watching
  devices/{id}/motor/commandFailure for the push side; WhatsApp is
  sent directly from the device itself, same as the existing WiFi-
  reconnect and OTA-failure alerts.
- Minor version bump (1.2.x -> 1.3.0), not a patch - this is a new
  feature, not a bug fix, per semver.

## [1.2.2] - 2026-08-26

- Reduced heap fragmentation risk in Cloud::publishDevice() and
  Cloud::publishMotor() - both run every 10 seconds for as long as the
  device is up, potentially months at a time. Replaced repeated
  String concatenation (each += can reallocate the whole buffer on
  the heap as it grows) with a single snprintf() into a fixed stack
  buffer per function - zero heap allocations for the JSON
  construction itself. Behavior-preserving: verified with a
  standalone comparison harness that the old and new logic produce
  byte-identical JSON across 8 cases (normal operation, no WhatsApp
  phone, offline device, escaped-quote name, max uptime value).
  Everything else in cloud.cpp (OTA, remote commands, path setup)
  deliberately left as String - those paths are one-time or rare/
  human-triggered, not a real fragmentation risk.

## [1.2.1] - 2026-08-25

- Fixed two real-hardware issues found while field-testing v1.2.0's CT
  motor feedback:
  - Motor stayed stuck reporting RUNNING forever after a real STOP,
    because the OFF threshold (0.05A) sat an order of magnitude below
    this CT's actual idle noise floor (~0.4A at this calibration's
    gain). Raised to 0.8A.
  - A single noisy 400ms RMS window could spike over the RUNNING
    threshold with nothing actually drawing that current, firing a
    false RUNNING report. Now requires 3 consecutive over-threshold
    windows before committing to RUNNING (~1.2s added latency); OFF
    stays immediate since there's no reason to delay reporting a real
    stop.
- See include/current_sensor.h for the full reasoning and the
  real-hardware Serial logs both fixes were verified against.

## [1.2.0] - 2026-08-24

- Added CT-based motor feedback: one YHDC SCT013-030 (built-in burden,
  voltage-output, 30A/1V rated) on ADC1 GPIO34, with an R8/R9/C3 network
  biasing and AC-coupling its output for the ADC - see the "Current-sensor
  feedback" section in README.md for the full circuit and a note on a
  redundant external resistor (R6) worth removing from the current build.
- Calibration constant uses the CT's datasheet-rated 30A/1V ratio; kept for
  Serial/debugging.
- Motor feedback changes to RUNNING only when measured current is > 2 A.
- Motor feedback returns to OFF at the calibrated near-zero/noise floor.
- Current amperage is no longer published to Firebase; only motor state is synchronized.

# Changelog

All notable changes are documented here. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/), versioning follows
[Semantic Versioning](https://semver.org/): PATCH for fixes, MINOR for
new backwards-compatible features, MAJOR reserved for a genuine 1.0
(production-ready) release.

This is the **Motor Control** module's changelog. Furrow is planned to
grow additional modules (see README.md Roadmap) - if/when those get
substantial enough to version independently, they'll likely get their
own changelog rather than sharing this one.

## [1.1.0] - 2026-08-12

### Added
- WhatsApp alert for connectivity outages: if WiFi/Firebase drops and
  reconnects after 30s or longer, sends "⚠️ [name] was offline for
  Xs - just reconnected". Uses purely local tracking (the ESP32's own
  clock, no network needed while counting) -
  `Network::lastDisconnectDurationMs()`, read right after the
  existing reconnect event fires. Only covers drops while the device
  stays powered - see the next item for genuine power loss.
- **Real power-loss WhatsApp alerts, including when the device is
  fully off:** a Firebase Cloud Function (`functions/index.js`,
  `powerWatchdog`) checks every device's `lastSeen` age once a
  minute, independently of the device itself - the only way to catch
  a device that's genuinely lost power and has no way to report its
  own absence. Includes dedup (`powerAlertSent`, resets to `false`
  automatically every heartbeat) so it doesn't re-fire every run for
  the same outage.
  - `Cloud::publishDevice()` now also publishes `whatsappPhone`
    (per-device recipient) alongside the dedup flag
  - **Requires the Blaze (pay-as-you-go) Firebase plan** - Cloud
    Functions aren't available on the free Spark plan. See SETUP.md
    step 8
  - Tested the watchdog's decision logic against 5 scenarios (too
    recent, genuinely stale, already-alerted dedup, missing phone,
    never-reported) with mocked calls, plus a module-load smoke test,
    before committing - see `functions/test.js`
  - Originally built against GitHub Actions instead of Cloud
    Functions; moved after Whapi.cloud rejected requests from GitHub's
    shared runner IP pool with a 403 in production (confirmed via
    logs that the decision logic itself was correct - only the send
    was rejected). `.github/workflows/power-watchdog.yml` is kept as
    a manual-only diagnostic tool now (`workflow_dispatch`, no
    schedule), not part of the automated path - running both on a
    schedule risked duplicate alerts for the same outage

## [1.0.0] - 2026-08-12

First release under MAJOR version 1 - this repo's own semver
philosophy (see the top of this file) reserves that specifically for
a genuine, production-ready milestone, not just "another version
bump." What justifies it here: the core control loop (remote
Start/Stop via relay-simulated pushbuttons) is now confirmed working
on two different real starter types (DOL and Star-Delta, see the
entry below), the whole provisioning/multi-device/OTA/alerting stack
has been exercised for real, and - just as importantly - the scope
going into this release was deliberately simplified rather than left
sprawling.

### Changed
- **Scope simplification, not a limitation being discovered later:**
  dropped the planned current-sensor-based motor feedback (no CT
  clamp, no RMS current calculation) in favor of a much simpler
  approach - sharing the ESP32's own low-voltage power supply with
  the existing digital single-phasing preventer, so a preventer trip
  reads as the device going silent. The dashboard's existing "No
  Power" status (30s of silence, see the entry below) already covers
  this; see README.md's "Not done yet" for the current wiring status
  and Roadmap for where current-sensing may still land later,
  deliberately deferred rather than abandoned.

### Added
- Documented ESP32-to-relay wiring (exact pins, active-LOW behavior)
  directly in README.md, rather than only ever having lived in chat
  history
- Enclosure reference: a free, purpose-fit 3D-printable case for this
  exact ESP32 + dual-relay pairing

## Testing - confirmed working on both starter types - 2026-08-11

No code change - a real-world testing milestone worth recording.
Tested successfully on both a DOL and a Star-Delta starter. The
star-to-delta transition timing is handled entirely by the existing
starter hardware's own internal timer relay, the same as it would be
for a manual button press - confirms the firmware genuinely never
needs starter-type-specific logic, a direct consequence of the
pushbutton-simulation design rather than something that needed
separate testing/hardening per starter type.

## Dashboard deploy - automated via GitHub Actions - 2026-08-11

Tooling-only change, no firmware code touched - no version bump, same
reasoning as the earlier PWA conversion entry. `web/` now deploys to
Firebase Hosting automatically on every push to `main` that touches
`web/**` or `firebase.json`, via `.github/workflows/firebase-hosting-deploy.yml`.
Calls the Firebase CLI directly (`firebase deploy`), authenticated
non-interactively via a service account - see SETUP.md step 9 for the
one-time setup.

## Dashboard fix - real semver comparison for update badge - 2026-08-11

Dashboard-only, no firmware code touched - no version bump. The
"update available" badge used plain string inequality, which was
wrong in two ways: it would fire even when a device was running
something *newer* than the latest release (a locally-flashed dev
build), and it broke on double-digit versions ("0.2.0" vs "0.10.0"
compares wrong as strings). Replaced with a real per-component
version comparison, tested against 7 cases before committing.

## Dashboard - offline reinterpreted as "No Power" - 2026-08-11

Dashboard-only, no firmware code touched - no version bump. Hardware
simplification: voltage/phase-loss protection is being handled
entirely by the existing digital single-phasing preventer rather than
custom ESP32 sensing - the device's own power supply will be wired
through it, so if the preventer cuts power, the ESP32 goes dark too.

This makes prolonged silence a genuine proxy for "the preventer cut
power," not just a dropped WiFi connection - the offline threshold
tightened from 40s to 30s, and its display label changed from
"Offline" to "No Power" (internal status keyword/CSS classes
unchanged - purely a display-label mapping, nothing else needed to
change). Recovery is automatic and needs no special handling: once
the device reconnects and publishes again, it naturally reads as
"Online" through the same existing logic.

## [0.2.0] - 2026-08-11

### Added
- Remote factory reset: new `factory_reset` command (same channel as
  restart/shutdown/update) clears WiFi credentials, device name,
  owner email, and WhatsApp config all at once - `AppStorage::factoryReset()`
- Dashboard control for it, with a type-the-device-name confirmation
  rather than a plain OK/Cancel dialog, given how much more
  consequential this is than Restart - whoever reconnects and
  reprovisions the device next becomes its new owner
- Unlike Shutdown (deliberately never exposed - needs a physical
  power-cycle to recover), Factory Reset stays reachable in person:
  the device comes back up and immediately starts broadcasting its
  own `Furrow-Setup-XXXX` network again, just unreachable via
  Firebase until someone reconnects it locally

## [0.1.1] - 2026-08-11

### Fixed
- GPIO0 (BOOT) is also the ESP32 ROM bootloader's own strapping pin,
  sampled at the instant of power-on/reset before any firmware code
  runs. The WiFi-reconfigure check previously required the button
  already held at the start of `setup()` - meaning holding it through
  power-on (as documented) made the ROM enter USB download mode
  instead of ever starting this firmware at all. Since `setup()` could
  only ever run when GPIO0 was *not* held at reset, the old check
  could never actually succeed for a real person - not a rare edge
  case, non-functional by construction. Fixed by never sampling GPIO0
  until well after normal boot has safely completed, and waiting for
  a fresh press-and-hold (5s window, 3s hold) instead of requiring it
  already held.
- Real behavior change worth knowing: every boot now takes an extra
  5s (waiting through that window even when reconfiguring isn't
  wanted) - the unavoidable cost of checking this safely every time
  rather than only when actually used.

## [0.1.0] - 2026-08-11

Fresh start under the Furrow name, with a clean git history - the
project outgrew "Smart Motor Controller" as it's becoming the base for
a broader agriculture IoT platform, and along the way it made sense to
stop carrying real credentials in git history entirely (see the
Secrets section in README.md) rather than try to scrub years of
commits after the fact.

Functionally, this reflects everything working as of the rename:
WiFi/device provisioning via captive portal, multi-device/multi-owner
support via Firebase Auth + RTDB rules, manual OTA updates with live
progress reporting, optional WhatsApp alerts (Whapi.cloud), an
installable PWA dashboard, and an automated build+release pipeline via
GitHub Actions.

**Known limitation, unchanged from before the rename:** motor state is
still command-based, not read from real hardware feedback - see
README.md's "Not done yet" section.

---

## How to cut a release

1. Bump `Config::FIRMWARE_VERSION` in `include/config.h`
2. Add a new section at the top of this file describing what changed
3. Commit both together
4. Tag it: `git tag -a v0.2.0 -m "v0.2.0"` (match whatever version you just set)
5. Push the tag: `git push origin v0.2.0`
6. **That's it** — `.github/workflows/build-and-release.yml` picks up the
   tag push automatically, generates `secrets.h` from repository secrets,
   builds the firmware, and creates the GitHub Release with
   `firmware.bin` attached. Check the Actions tab if you want to watch
   it happen or debug a failed build.
7. The OTA-ready download URL is always:
   `https://github.com/ultranoob-5/furrow/releases/latest/download/firmware.bin`
   — GitHub keeps this pointed at whichever release was published most
   recently, so it doesn't need to change between updates.
