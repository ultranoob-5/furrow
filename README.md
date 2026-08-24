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
- Connectivity outages of 30s or longer get flagged with how long the
  device was gone, the moment it reconnects (covers WiFi/network
  drops while still powered)
- **Real power-loss alerts**, including when the device is fully off:
  a Firebase Cloud Function (`functions/index.js`) checks every
  device's last-seen time independently of the device itself, so it
  can catch a device that's genuinely lost power and has no way to
  report its own absence. Requires the Blaze (pay-as-you-go) Firebase
  plan - see SETUP.md step 8. Runs every 1 minute (Cloud Scheduler's
  granularity), not by the 30s threshold itself

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
- **Backend:** Firebase Realtime Database + Authentication (Email/Password),
  Cloud Functions (power-loss watchdog only, requires Blaze plan)
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

## Current-sensor feedback (1-channel)

The controller supports one YHDC SCT013-030 CT on ADC1 GPIO34. Per its
datasheet, this model has its sampling (burden) resistor built in - it's a
genuine voltage-output CT rated 30A RMS primary -> 1V RMS output (max input
60A), so no external burden is needed for the CT itself. R8/R9 bias the
signal to 1.65V and C3 AC-couples the CT's output onto that bias, so the ADC
always sees a valid, mid-rail-centered waveform. Sampling uses the same
RMS/digital-offset-filter approach as the Mottramlabs ESP32 4-channel
current-sensor firmware.

**Hardware note:** the CT_1 schematic this was built from also has a 100
ohm resistor (R6) wired across the CT's own output leads. Since this CT
already has an internal burden resistor, R6 is an extra load in parallel
with it - the CT's datasheet doesn't publish the internal burden value, so
there's no way to calculate exactly how much R6 pulls the output down from
the rated 1V/30A figure. Recommend removing/bypassing R6 so the CT's rated
output reaches C3 unloaded.

The current reading is used as the motor's source of truth: physical
starter-button START/STOP and remote START/STOP both become visible through
the same motor state. Whether or not R6 stays fitted, calibrate against a
clamp meter on a known motor current before treating the Amps value as
measurement-grade - it's only used internally for the 2A ON/OFF threshold,
never published to Firebase.

### Schematic

![CT sensor circuit: SCT013-030 into an R8/R9 bias divider and C3 AC-coupling capacitor, feeding the ESP32 ADC](docs/schematics/ct-sensor-circuit.jpg)

- **CT_1_wire1 / CT_1_wire2** - the two output leads of the SCT013-030
  clamp.
- **R8, R9 (10K each)** - form a divider between 3.3V and GND, biasing
  the ADC input to 1.65V (mid-rail) so the AC waveform swings within the
  ESP32's 0-3.3V ADC range instead of clipping at ground on every negative
  half-cycle.
- **C3 (10uF)** - AC-couples CT_1_wire1 onto that 1.65V bias, blocking any
  DC offset from the CT while passing the current-proportional AC signal
  through.
- **R6 (100 ohm)** - currently wired across CT_1_wire1/CT_1_wire2. As
  noted above, this CT already has its own burden resistor built in, so R6
  is redundant and recommended to be removed - see the hardware note
  above.
- **"Input for esp32"** - connects to `Config::CURRENT_ADC_PIN` (GPIO34,
  ADC1) in `include/config.h`.

### Wiring it up

1. **Clamp the CT around one motor-side conductor.** Open the SCT013-030
   and close it around a single phase wire feeding the motor (not both
   wires of a cable, or it reads near-zero - the clamp needs to sense one
   conductor's magnetic field only). The white dot/arrow on the clamp
   body, if present, doesn't matter for current magnitude, only for sign,
   which this firmware doesn't use.
2. **Wire the CT's two leads to CT_1_wire1 and CT_1_wire2** in the
   schematic above - polarity doesn't matter for an AC measurement.
3. **Build the bias/coupling network** (R8, R9, C3) between those leads
   and the ESP32, exactly as drawn. If R6 is still fitted from an earlier
   build, remove it per the hardware note above.
4. **Connect the junction after C3 ("Input for esp32") to GPIO34** on the
   ESP32 - this is `Config::CURRENT_ADC_PIN` in `include/config.h`, and
   stays on ADC1 so it keeps working alongside WiFi.
5. **Power the bias network from the ESP32's own 3.3V and GND** - don't
   share this with anything mains-side; the CT clamp itself is the only
   part of this circuit anywhere near the motor wiring, and it's
   inherently isolated (that's the whole point of a clamp-on CT).
6. **Flash the firmware and open the Serial monitor** (115200 baud). With
   the motor off, you should see a reading near 0.00A; with it running,
   `[Current] X.XX A RMS | Motor: RUNNING` every 10 seconds. If it doesn't
   move at all, double check the CT is clamped around only one conductor
   and that CT_1_wire1/wire2 are actually connected (an open CT input
   floats and reads noise, not a clean zero).
7. **Calibrate:** compare the Serial reading against a clamp meter on the
   same conductor while the motor runs. If it's consistently off by a
   fixed ratio, adjust `CT_RATED_PRIMARY_A`/`CT_RATED_OUTPUT_V` in
   `include/current_sensor.h` to match reality rather than the datasheet
   figure - this matters most if R6 ends up staying in the circuit, since
   that's what would introduce the mismatch.
