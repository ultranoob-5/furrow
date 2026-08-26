
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
