## [1.6.3] - 2026-10-18

Dashboard, flasher, hosting headers and a small firmware fix. No rules or Cloud Functions change.

- **No Reload After Signing In**: On a slow connection the "Couldn't reach the server" screen could appear while sign-in was still working; once the app came up, that screen's retry timers still reloaded it about 15 seconds later, and again whenever the phone regained signal. They now stand down as soon as sign-in resolves.
- **Live Data After Any Sign-Out**: Only the Logout button cleaned up the device listener, so after a session expired (or a password change elsewhere) the next sign-in showed no live data. Every sign-out now does the cleanup.
- **Update Button**: The device keeps reporting "failed" until it reboots, so the button flipped between "Update failed" and normal every second, and a retry was immediately shown as failed again. A failure now shows once, a retry waits for the device's new progress, and "Initializing update..." stays with the device it was sent to when you switch devices.
- **Notifications**: "Send test notification" is no longer disabled forever after one use, and its result ("Test sent - check your notifications") no longer disappears after a second.
- **App Update Banner**: No longer stuck on "Downloading update in background..." on a first visit or after a failed install; the Update button and the reload logic now share the same state.
- **Firmware 1.6.3 - Commands Can't Push Each Other Out**: The device kept one pending command, so a settings sync arriving right after a Start (a quick second action, or saving a setting) replaced the Start. Settings syncs now have their own flags; Start/Stop/Restart/Update are never replaced by them.
- **Flasher - Way Back to the Dashboard**: A "Back to dashboard" link at the top, and when the flashing window closes, a "Finished flashing?" box with the next steps (power it up, join its Furrow-Setup WiFi from your phone) and a Go to dashboard button. The last step now points to the dashboard too, not just SETUP.md.
- **Flasher**: The firmware download address is built from the site's own Firebase config instead of a hard-coded project id.
- **Content-Security-Policy (Report-Only)**: A full policy is now sent in report-only mode - nothing is blocked; any violations show in the browser console so it can be enforced later.
- **PWA Cache Invalidation**: Service worker cache `furrow-dashboard-v18`, PWA `v1.6.3`.

## [1.6.2] - 2026-10-18

Dashboard, database rules, Cloud Functions and a small firmware fix. **Publish the new rules before this dashboard goes live** (the `command/issuedAt` rule) - without it, Start/Stop are rejected.

- **Safety - Old Commands Can't Run Later**: A Start/Stop/Restart/Update/Factory reset tapped while the phone or the device was offline used to wait and run whenever both reconnected - possibly hours later. Commands now carry the time they were sent: the database rules reject one that's more than a minute old (a tap the phone queued without signal), and `powerWatchdog` clears any command the device hasn't picked up within a minute (device offline). The dashboard also refuses to send while the phone has no internet, and says so.
- **"No Power" Only When It's True**: When the phone itself was offline, or the app had just come back from the background, old data made the dashboard say "No Power - electricity cut at starter panel". It now shows "Checking…" and the last known pump state until live data arrives.
- **Settings No Longer Snap Back**: A schedule time or auto-resume delay picked while the feature was off reset within a second, so Enable saved the old values. They're now saved straight away (and used when the feature is turned on), and the settings' messages are no longer wiped every second.
- **WhatsApp Number**: The number box no longer gets overwritten while you edit it. A 10-digit Indian mobile number (or one starting with 0 or +91) is saved with the 91 country code - before, it was accepted without it and alerts never arrived.
- **Stop Always Available**: Stop is enabled whenever the device is online, even if the pump reads OFF (a wrong reading, or a pending auto-resume start), and while a Start is still on its way.
- **Firmware 1.6.2**: A Stop sent to an already-stopped pump no longer makes the next stop at the panel get reported as "from the app".
- **PWA Cache Invalidation**: Service worker cache `furrow-dashboard-v17`, PWA `v1.6.2`.

## [1.6.1] - 2026-10-18

Firmware only - update from the dashboard or flash. No rules, Cloud Functions or dashboard change.

- **Safety - Internet Drop No Longer Restarts a Stopped Pump**: With auto-resume on, an internet drop of 30s+ while pumping made the cloud watchdog record "was running". If the pump was then stopped at the panel, the device acted on that record when the internet came back and started it again. The device no longer uses the cloud record at all - auto-resume after a real power cut comes from the device's own memory (reliable since 1.6.0).
- **Auto-Resume Respects the Schedule**: With a schedule enabled, auto-resume no longer restarts the pump outside the schedule's ON window (for example when power returns after the OFF time), which used to leave it running until the next day's OFF time. Needs the clock, so it applies once the device has had internet since power-up.
- **Restart / Factory Reset / Shutdown Can't Repeat**: These cleared the command and rebooted after a fixed 1s pause, without waiting for the clear to reach the server - if it hadn't, the device ran the same command again after booting. It now waits (up to 5s) for the server to confirm.
- **WhatsApp Number From Setup Kept**: A number typed on the WiFi setup page was erased the first time the device went online, because the dashboard had never set one. The device now only clears it when it's cleared from the dashboard.
- **Owner Email Case**: The owner email is stored in lowercase, so a capital letter typed at setup no longer locks the owner out (Firebase emails are lowercase). Also applied to already set-up devices.
- **Scheduled OFF Always Presses Stop**: At the OFF time the device presses Stop even when it reads the pump as already off, so a wrong reading can't skip it.
- **Schedule Edits Same Day**: Moving the OFF time to later today now stops the pump today, even if the old OFF time already fired - before, a running pump wasn't stopped until tomorrow. (A changed ON time still waits for tomorrow if today's start already happened, so an edit never adds a surprise start.)

## [1.6.0] - 2026-10-17

Firmware only (1.4.4 -> 1.6.0) - update from the dashboard or flash. No rules, Cloud Functions or dashboard change.

- **Safety - Device Checks Auto-Resume Too**: When the cloud watchdog had recorded the pump as running before an outage, the firmware started the auto-resume countdown without checking the device's own auto-resume setting. It now checks first, the device-side half of the 1.5.5 fix.
- **Turning Auto-Resume Off Cancels a Countdown**: Disabling auto-resume while a countdown was running didn't stop it - the pump still started when it ran out. It now cancels the countdown and forgets the outage.
- **Auto-Resume Survives a Second Power Cut**: The device cleared its "was running" record as soon as it booted, so if power dropped again during the countdown (common when supply flickers back), the next boot forgot the pump had been running. The record now stays until the start is attempted. One attempt per outage: if the pump doesn't start (tripped overload, no supply), it won't try again at the next power-up.
- **Running Pump Remembered After a Restart or Update**: A pump that kept running through a device restart or OTA update was never recorded as running, so a power cut afterwards wouldn't auto-resume it. The device now records the real state at startup.
- **No False Alerts After a Restart With the Pump Running**: The first current reading after boot counted before a running motor had been confirmed (3 readings), so the device published OFF and then a "manual" start - two wrong alerts after every restart or update while pumping. It now waits until the state is decided.
- **No Relay Blip at Boot**: The relay pins were switched to output before being set to off, driving Start/Stop on for an instant at every boot. They're now set to off first, at the very start of boot.
- **Setup Page Escaping**: The WiFi setup page escapes the saved device name, email and phone number, so a name containing a quote can't break the page.
- **Cleanup**: Removed unused `Network::hasInternet()`.

## [1.5.5] - 2026-10-14

Cloud Functions, dashboard text and docs - no firmware or rules change.

- **Safety - No Auto-Start With Auto-Resume Off**: `powerWatchdog` recorded `motor/stateBeforeOutage: "RUNNING"` for every running pump that lost power, and the firmware starts the motor from that value when power returns - so a pump with auto-resume DISABLED started by itself after a power cut. The watchdog now only records it when the device's auto-resume is enabled (otherwise it clears it). Covered by new `functions/test.js` cases. (The matching firmware-side check ships in the next firmware release.)
- **One Alert Per Power Cut**: The watchdog's `motor/state -> OFF` write also triggered `onMotorStateChanged`, so a power cut sent both "Power Cut Alert" and "turned OFF". The watchdog now tags that write `stoppedVia: "power-loss"` and the motor-change alert skips it. Wording moved into a pure `motorAlertText()` helper with tests.
- **Docs**: SETUP.md section 3 no longer shows an old inline rules block (it had `".read": true`); it points to `database.rules.example.json` as the single source of truth and summarizes what the rules enforce. Troubleshooting now matches the dashboard (action buttons need the device online) and no longer describes motor state as command-based.
- **Schedule Text**: The dashboard and SETUP.md now say what's actually true - schedules run on the device even if the internet drops, but after each power-up it needs internet once to set its clock.
- **PWA Cache Invalidation**: Service worker cache `furrow-dashboard-v16`, PWA `v1.5.5`.

## [1.5.4] - 2026-10-13

Web only - no firmware or rules change.

- **Self-Hosted Flasher Library**: `flash.html` loads ESP Web Tools 10.4.0 from this site (`web/vendor/`, copied from the exact version pinned in `package.json` by `npm run build`) instead of unpkg.com. The flasher writes firmware to hardware, so a compromised CDN must not be able to change what it does. The service worker no longer caches unpkg.com.
- **Firebase SDK 13 Bundle Committed**: `web/firebase-bundle.js` / `web/sw-firebase.js` rebuilt locally with Firebase 13.0.0, matching what the hosting deploy already builds and serves.
- **App Check (Web, Monitoring)**: The dashboard initializes Firebase App Check with reCAPTCHA Enterprise when `appCheckSiteKey` is set in its config (from the new `FIREBASE_APPCHECK_SITE_KEY` GitHub secret), so its Auth/Database/Functions requests carry App Check tokens. Missing key or any App Check failure is non-fatal. Authentication and Realtime Database must stay unenforced: the ESP32 firmware can't produce App Check tokens. See SETUP.md section 14.
- **Custom Domain**: The dashboard is now live at https://furrow.in (Firebase Hosting custom domain); `furrow-123098.web.app` keeps working. README link updated.
- **PWA Cache Invalidation**: Service worker cache `furrow-dashboard-v15`, PWA `v1.5.4`.

## [1.5.3] - 2026-10-13

Web dashboard fixes - no firmware or rules change.

- **Online / Updating Flicker on Devices With a Wrong Clock**: Device age was computed as the browser's clock minus the server-stamped `lastSeen`, so a PC or phone whose clock ran ~6s+ fast flipped Online <-> Updating on every heartbeat (confirmed on a PC 13.3s fast while the device's heartbeats were arriving every 10.0s). The dashboard now corrects with Firebase's `.info/serverTimeOffset`.
- **Device List Denied After Verifying Email**: The first load right after verifying an email could fail with `permission_denied`, because the device listener attached before the database connection had picked up the refreshed (now-verified) token. The dashboard now refreshes the token and retries once before showing an error; the first attempt's "Still not connected" timer no longer fires over a retry that succeeded.
- **Service Worker Offline Fallback**: When a page request failed and nothing was cached, the service worker handed the browser nothing (`Failed to convert value to 'Response'`). It now falls back to a cached copy ignoring the query string, then a small offline page with a Retry button.
- **PWA Cache Invalidation**: Service worker cache `furrow-dashboard-v14`, PWA `v1.5.3`.

## [1.5.2] - 2026-10-13

Security hardening (web, rules, Cloud Functions) - no firmware change.

- **Device Account Locked Down**: The shared device login ships inside the public release firmware, so the database rules now only let it do what the firmware actually does: it can no longer list every device, can only write `"none"` to `command/action` (so it can't start or stop pumps), can only clear `stateBeforeOutage`, and can no longer write `schedule`. Copy into your real `database.rules.json` and publish.
- **Clickjacking Protection**: Hosting now sends `X-Frame-Options: DENY` and `frame-ancestors 'none'`, so the dashboard can't be embedded in another site to trick an owner into pressing Start/Stop. Also adds `nosniff`, `Referrer-Policy`, and a `Permissions-Policy` (Web Serial for the flasher is unaffected).
- **Test Notification Hardened**: `sendTestNotification` now requires a verified email, ownership of the named device, and an FCM token already registered under that device; title and body are fixed server-side.
- **Firmware Proxy Cost Cap**: `firmwareProxy` limited to 5 instances.
- **PWA Cache Invalidation**: Service worker cache `furrow-dashboard-v13`, PWA `v1.5.2`.


## [1.5.1] - 2026-10-13

Web dashboard only - no firmware change.

- **Create Account**: Owners can sign up from the dashboard's sign-in screen instead of being added by hand in the Firebase Console.
- **Email Verification Gate**: New (and existing, not-yet-verified) accounts must verify their email before the dashboard opens. Includes resend with a 30s cooldown, and an automatic re-check when the person returns to the tab after clicking the link.
- **Change Password**: Signed-in owners can change their password from a dialog next to Logout. Confirms the current password first (Firebase requires a recent sign-in), clears typed passwords whenever the dialog closes, and offers a reset link if the current password is forgotten. The footer now also shows which account is signed in.
- **Account Controls Always Reachable**: The footer, Change password and Logout moved out of the device panel into their own block, so an account with no devices yet can still log out (the panel is hidden in that case, which previously left no way to sign out). That state now shows "No devices are registered to your account yet" instead of the generic "No devices found" message, and the stale "Loading devices..." placeholder is cleared.
- **Forgot Password**: Self-service password reset by email. Shows the same confirmation whether or not an account exists, so the form can't be used to discover who has an account.
- **Security - Database Rules**: All owner read/write rules in `database.rules.example.json` now also require `auth.token.email_verified == true`. Without this, open sign-up would let anyone register with a device owner's email before the owner does and control their pump. Copy these into your real `database.rules.json` and deploy them.
- **Friendlier Auth Errors**: Firebase error codes are mapped to plain-language messages.
- **PWA Cache Invalidation**: Bumped service worker cache to `furrow-dashboard-v12` and PWA version to `v1.5.1`.

## [1.4.3] - 2026-09-12

- **Serial Transmission & Clock Stability**: Added `Serial.flush()` to ensure all bytes leave the hardware UART FIFO before periodic Wi-Fi SSL transmissions occur, and disabled Wi-Fi modem sleep (`WiFi.setSleep(false)`) to eliminate dynamic APB bus clock scaling and RF wake-up jitter that caused serial output corruption.
- **Smartphone & Tablet Responsiveness Matrix**: Comprehensive optimization across 35 device viewports and DPI modes (OnePlus Nord CE4, Google Pixel series, Nothing Phone, OnePlus flagships, Redmi series, iPad family, Samsung Galaxy Tab family, and Android compact tablets) with 0px horizontal document scroll and safe-area insets (`viewport-fit=cover`, `100dvh`).
- **Tablet CSS Grid Overview**: Implemented adaptive 2-column grid at $\ge 640\text{px}$ and 3-column grid at $\ge 900\text{px}$ for device overview chips with proportional container scaling (`max-width: 560px / 620px`).
- **WhatsApp Action Row Optimization**: Expanded action row breakpoint to 540px, ensuring wide and high-density phones display full-width telephone input and accessible 44px buttons without row compression.
- **PWA Cache Invalidation**: Bumped service worker cache to `furrow-dashboard-v10` and PWA version to `v1.4.3` to ensure instant cache invalidation on installed client PWAs.

## [1.4.2] - 2026-09-12

- **WhatsApp Recipient Clearing & Synchronization Fix**: Resolved issue where clearing WhatsApp number failed due to `status/` write permission errors and firmware skipping NVS wipe on empty/null strings. Updated dashboard to set `whatsappPhone` to `""`, updated firmware `fetchWhatsAppPhone` to wipe NVS key on empty/null, and ensured boot sync always runs without skipping when local credentials exist.

## [1.4.1] - 2026-09-12

- **Device Display Name Synchronization**: Resolved device name divergence where dashboard renamed devices (e.g. "Well Pump") triggered alerts under the factory captive-portal name (e.g. "Farm Pump").
- **Cloud Functions Display Name Resolution**: Added `resolveDeviceName` across all notification triggers (`onMotorStateChanged`, `onPowerRestored`, `onMotorCommandFailed`, and `powerWatchdog`) prioritizing user-defined `displayName` over `status/name`.
- **Firmware NVS Name Sync**: Added `AppStorage::setDeviceName()` and cloud sync logic on boot, reconnect, and via `sync_name` remote command so the ESP32 synchronizes its local storage and status reports with `displayName`.
- **Web Dashboard Rename Dispatch**: Updated `startRename()` to automatically dispatch the `sync_name` action to the device whenever a device is renamed in the UI.
- **CodeQL DOM XSS Hardening**: Eliminated all `innerHTML` sinks in OTA button and chip status renders using safe DOM element and text node manipulation.

## [1.4.0] - 2026-09-11

- **Instant Remote NVS Auto-Resume Synchronization**: Added `sync_auto_resume` remote command handler in firmware and connected dashboard toggle/delay changes to dispatch it immediately. Ensures ESP32 NVS flash storage holds the active auto-resume configuration and pre-outage motor state prior to mains power failure, enabling true offline local reboot autonomy without internet connectivity.
- **Dashboard WhatsApp Alerts & Dual-Path Fallback**: Added a WhatsApp alerts configuration card directly in the web dashboard settings. Cloud functions and firmware now synchronize WhatsApp recipient numbers both from `devices/{id}/whatsappPhone` and `devices/{id}/status/whatsappPhone` with automatic NVS storage and safe handling of unset keys.
- **Power-Loss Overview Priority**: Fixed device card status hierarchy in `dashboard.html` to prioritize offline/power-loss detection over stale `RUNNING` indicators, immediately showing "No Power" upon device disconnect.
- **DevMode NVS State Persistence**: Ensured `Motor::start()` and `Motor::stop()` immediately record `RUNNING` and `OFF` states to NVS in development mode for offline recovery testing.

## [1.3.9] - 2026-09-11

- **Motor-Guarded Power Loss Alerts**: Updated Cloud Functions `powerWatchdog` to only send power loss notifications (WhatsApp and FCM Push) when the motor was actively `RUNNING` at the time of the outage. If the pump is already `OFF`, the outage does not interrupt irrigation, avoiding nuisance alert spam when mains power flickers on an idle pump.
- **Offline Autonomy & Dynamic Schedule / Auto-Resume**: Decoupled automation engine (`Automation`) and persisted all schedule timings, auto-resume parameters, and last motor states in ESP32 NVS flash storage (`AppStorage`), guaranteeing full local operation without internet dependency.

## [1.3.8] - 2026-09-10

- **Remote Start/Stop Attribution Decoupling**: Decoupled state transition attribution
  flags (`remoteStartPending`, `remoteStopPending`) from the 30-second watchdog
  confirmation timer in `src/cloud.cpp`. Resolves an issue where command confirmation
  checks could prematurely clear the active command status before the main loop detected
  the motor state change, ensuring remote commands triggered via web or mobile app are
  accurately attributed as `"remote"` rather than falsely falling back to manual starter panel events.
- **Streamlined Command Execution**: Removed premature motor state writes during
  remote command dispatch in `handlePendingCommand()`, guaranteeing all state reporting
  reflects confirmed sensor feedback.

## [1.3.7] - 2026-09-10

- **Autonomous On-Device Irrigation Schedule**: Migrated the daily scheduled
  on/off watchdog directly onto the ESP32 firmware using hardware SNTP time
  synchronization in IST (UTC+5:30). Completely removed the 1-minute
  `scheduleWatchdog` Cloud Function cron job, saving ~43,200 runs/month.
- **Active Internet Connectivity Guard**: Ensures scheduled turn on and
  turn off actions only fire when the device has active internet connectivity
  (`Network::hasInternet()` via DNS reachability), preventing the pump from
  operating blindly offline.
- **Strict Manual-Override Protection**: Uses RTDB tracking (`lastOnFiredDate`,
  `lastOffFiredDate`) ensuring scheduled events fire at most once per calendar
  day. Manual start or stop at the physical starter panel stays in control and
  is never overridden by the schedule on the same day.
- **Elder-Friendly Unified Notifications & WhatsApp Parity**: All notifications
  have been simplified with elder-friendly phrasing and visual status cues (🟢, 🔴, ⚡, ⚠️, ✅)
  without technical jargon. Achieved 100% parity between Firebase Cloud Messaging
  and WhatsApp across all channels: remote on/off via web, manual starter panel buttons,
  daily irrigation schedule, power restoration, auto-resume, and command failures.
- **Bi-Directional State Transition Attribution (`stoppedVia`)**: Extended firmware
  state reporting with `stoppedVia` (`remote`, `schedule`, `manual`), allowing Cloud
  Functions to accurately dispatch the correct context for motor stop events.
- **Interactive Analog Clock Picker**: Replaced the 15-minute interval steppers
  in the web dashboard with an interactive analog clock face picker dial supporting
  smooth touch/drag, 1-minute precision, ±1m nudge buttons, and instant preset chips.
- **Notification Click-to-Open**: Added a `notificationclick` handler in the Service
  Worker (`sw.js`) and attached target device URLs to FCM push payloads, ensuring
  that clicking a push notification on phone or desktop immediately brings up or
  launches the web app and selects the alerted motor.
- **Cloud Functions Cleanup**: Cleaned up legacy cloud watchdog helpers from
  `functions/index.js` and fixed dormant unreferenced variables in `onPowerRestored`.

## [1.3.6] - 2026-09-05

- **Autonomous On-Device Auto-Resume**: Moved post-outage motor auto-resume
  countdown and relay triggering directly onto the ESP32 firmware upon boot.
  Eliminated the 1-minute `autoResumeWatchdog` Cloud Function cron job, saving
  1,440 daily cloud function executions (~43,200 runs/month).
- **Dynamic RTDB Delay & Live Cancellation**: Fetches user-configured delay
  (1–10 minutes) directly from `/devices/{id}/autoResume` on boot. Listens
  for live `cancel_auto_resume` commands from the dashboard and immediately
  aborts pending timers on manual panel stop, remote stop, or motor state change.
- **Hardware FPU Float Operations**: Switched current sensor math to native
  32-bit `float` (`sqrtf`) leveraging ESP32 Xtensa LX6 hardware FPU.

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
