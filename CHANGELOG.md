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
