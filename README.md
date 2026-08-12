<p align="center">
  <img src="web/icons/icon-512.png" width="96" alt="Furrow logo">
</p>

<h1 align="center">Furrow</h1>
<p align="center"><i>Agriculture IoT platform - built on ESP32 + Firebase</i></p>

Furrow started as a single retrofit: remote control for one farm
pump's 3-phase Star-Delta starter. It's now the base for a broader
agriculture IoT platform - same firmware/dashboard/backend
architecture, extended module by module rather than rebuilt each time.

**Motor Control is the first module, and the only one built so far** -
everything below describes that module specifically. See
[Roadmap](#roadmap) for what's planned to join it.

**Live dashboard:** [furrow-123098.web.app](https://furrow-123098.web.app)

---

## Module: Motor Control

Remote monitoring and control for a 3-phase Star-Delta (or DOL) motor
starter.

An ESP32, wired to a 2-channel relay, simulates the physical Start and
Stop pushbuttons on an existing starter panel. The ESP32 never touches
the main contactor coils directly - it only interfaces with the same
low-voltage control loop a person would use by pressing the buttons
themselves.

**Tested working on both a DOL and a Star-Delta starter.** The
star-to-delta transition timing is handled entirely by the existing
starter hardware's own internal timer relay, exactly as it would be
for a manual button press - the firmware never needs to know or care
which starter type it's talking to, a direct consequence of only ever
simulating the pushbuttons rather than driving any contactor logic
itself.

- **No rewiring of the starter panel.** It's a retrofit, not a
  replacement - manual operation at the panel keeps working exactly
  as before.
- **The ESP32 stays electrically isolated from mains.** It reads and
  writes only low-voltage signals; anything touching the 415-440V
  side was deliberately designed around non-invasive or isolated
  components. See [A note on safety](#a-note-on-safety).

Control and monitoring happen through a web dashboard backed by
Firebase - no app to install, works on desktop or phone.

### Wiring: ESP32 → relay module

| ESP32 pin | Relay module pin | Notes |
|---|---|---|
| GPIO26 | IN1 (Start channel) | Active-LOW - pulled LOW pulses the relay on |
| GPIO27 | IN2 (Stop channel) | Active-LOW, same as above |
| 5V | VCC | |
| GND | GND | |

The relay's Start/Stop channels wire into the starter panel's existing
low-voltage control loop, in parallel with the physical pushbuttons -
never into the main contactor coils or anything carrying 415-440V
directly. See [A note on safety](#a-note-on-safety).

### Enclosure

[ESP32 (WROOM) + dual-channel relay module case](https://makerworld.com/en/models/681746-esp32-wroom-dual-channel-relay-module-case)
by Tomek550 - free 3D-printable, fits this exact hardware pairing.
Both boards mount with M3 screws, snap-on lid, cutouts for the USB-C
port, relay cables, and serial pins. ~1.2h print time.

---

## Features

**Control**
- Remote Start/Stop, simulating the physical pushbuttons
- Remote Restart (Shutdown exists in firmware but isn't exposed in
  the dashboard - the one command with no remote way back)
- Manual OTA firmware updates from the dashboard, with live progress
  (percentage + phase) on both the dashboard and Serial

**Connectivity & setup**
- WiFi and device setup via a SoftAP + captive portal
  (`Furrow-Setup-XXXX`) - no laptop/USB needed after the initial
  flash, works the same on iPhone, Android, and desktop
- Device name, owner email, and optional WhatsApp alert recipient are
  also set through the same portal - one firmware binary works for
  any device or owner
- Automatic WiFi/Firebase reconnect with an auth watchdog

**Multi-device, multi-user**
- MAC-based device identity - multiple boards run identical firmware
  without colliding in Firebase
- Each device is tied to an owner's email, enforced by Firebase Auth
  + RTDB rules - friends can share one Firebase project and each only
  see/control their own device(s)

**Dashboard**
- Installable PWA - works like an app from your phone's home screen
- Live status with a server-authoritative "last seen" timestamp -
  accurate from the first page load, not dependent on a browser
  having been open to observe it
- Graded connection state, including "No Power" after 30s of silence
  (a real power-loss proxy, not just a generic offline label - see
  [Not done yet](#not-done-yet) for how this ties to the power
  supply wiring)
- Device rename, inline
- "Update available" badge when a device's firmware is behind the
  latest release

**Alerts**
- Optional WhatsApp alerts (via [Whapi.cloud](https://whapi.cloud))
  for device/connectivity health events - opt-in per device, no
  effect on anything else if left unconfigured

**Release process**
- Semantic versioning, documented in [CHANGELOG.md](CHANGELOG.md)
- Firmware: tag a version → GitHub Actions builds it and publishes a
  GitHub Release with the `.bin` attached, automatically
- Dashboard: push a change to `web/` on `main` → deploys to Firebase
  Hosting automatically, independent of firmware releases
- The dashboard's OTA update always points at the latest published
  firmware release
- No credentials of any kind are ever committed - see
  [Secrets](#secrets)

---

## Not done yet

- **Phase-loss / power monitoring - physical wiring pending.** The
  ESP32's own power supply is being routed through the existing
  digital single-phasing preventer, so a trip there cuts the ESP32's
  power too - the dashboard already treats a device going silent for
  30s as "No Power" for exactly this reason (not a WiFi hiccup). The
  logic is done; the physical wiring isn't confirmed yet.
- **Motor-fault WhatsApp alerts.** The alert system exists (device
  health, OTA events, and "No Power" once the wiring above is
  confirmed); alerts for the motor itself (started/stopped
  unexpectedly) aren't planned for v1 - see below.
- **Remote factory reset.** Implemented as a dashboard/remote command
  (see CHANGELOG.md 0.2.0) - not yet verified on real hardware, so
  still listed here until that's confirmed.

**A deliberate v1 scope decision, not an oversight:** motor state is
command-based (assumed from the last relay pulse), not read from real
current flow - a manual Start/Stop press at the physical panel isn't
reflected on the dashboard. A non-invasive current sensor was
originally planned to close this gap; v1 intentionally ships without
it in favor of a simpler, well-tested system. "No Power" detection
(above) covers the failure mode that actually matters most for a farm
pump - the supply going out entirely - without needing extra sensor
hardware. Real current-based feedback may still happen later; see
[Roadmap](#roadmap).

## Roadmap

Future modules under consideration, not started:
- Well water level monitoring
- Irrigation gate valve control
- Flow rate sensing, energy/runtime tracking
- Real motor feedback via a non-invasive current sensor - deliberately
  dropped from v1 in favor of simplicity (see "Not done yet" above),
  not ruled out permanently

---

## Getting started

Full step-by-step setup is in **[SETUP.md](SETUP.md)** - roughly
20-30 minutes, mostly one-time Firebase Console clicks. Shape of it:

1. Create a Firebase project (Realtime Database + Authentication)
2. Copy `include/secrets.example.h` → `include/secrets.h`, fill in
   your values (never committed - see [Secrets](#secrets))
3. Flash once via USB (PlatformIO)
4. Connect to `Furrow-Setup-XXXX`, complete setup from a browser
5. Open the dashboard, sign in, done

## Secrets

Nothing that grants write access or costs money on someone else's
account is ever committed:

- `include/secrets.h` (firmware) and `web/firebase-config.js`
  (dashboard) are both gitignored - copy from their `.example`
  templates for local builds
- CI generates both, plus `.firebaserc`, at build/deploy time from
  GitHub Actions repository secrets - never appear in any commit or log

**One exception worth understanding:** Firebase Web API keys *are*
meant to be public in client-side code - Google's security model puts
the real boundary at Firebase Security Rules, not at hiding that key.
What genuinely matters to keep secret is the device account password
and the WhatsApp sender token - both live in `secrets.h`.

---

## Tech stack

- **Firmware:** ESP32 (Arduino, PlatformIO), FirebaseClient (mobizt)
- **Backend:** Firebase Realtime Database + Authentication (Email/Password)
- **Dashboard:** Vanilla JS + Firebase Web SDK, installable PWA
- **CI/CD:** GitHub Actions - firmware release on tag push, dashboard
  deploy on `web/` push to `main`

---

## A note on safety

This project intentionally keeps the ESP32 away from anything above
low voltage. Relay outputs simulate pushbuttons rather than switching
contactor coils directly, and power monitoring works by sharing the
ESP32's own low-voltage supply with the existing digital
single-phasing preventer, rather than adding new sensors or tapping
into anything carrying mains voltage directly. If you're adapting
this for your own panel, treat anything touching mains voltage as
electrician territory, not a wiring diagram to follow alone - this
applies to every future module too, not just Motor Control.
