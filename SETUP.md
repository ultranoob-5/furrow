# Setup Guide - Motor Control module

This covers setting up the Motor Control module end to end: Firebase
project, secrets, flashing, and first-boot provisioning. Expect this
to take 20-30 minutes the first time, mostly Firebase Console clicking.

This project uses **one shared Firebase project** for every device and
owner (you and anyone you're setting this up for) - not a separate
project per person. Each person gets their own login and only ever
sees their own device(s); see README.md for how that isolation works.

---

## What you'll need

- An ESP32 dev board, flashed once via USB
- [VS Code](https://code.visualstudio.com/) with the [PlatformIO extension](https://platformio.org/install/ide?install=vscode)
- A Google account (for Firebase)

---

## 1. Create the Firebase project (once, for the whole platform)

Skip this if a Furrow Firebase project already exists and you're just
adding a device/owner to it - jump to step 4.

**If you're starting a genuinely new, independent Furrow deployment**
(not adding to one that already exists), creating the project in
Firebase Console is only half the work - three files still point at
the *original* project by default and need updating to point at
yours instead:

| File | What to change |
|---|---|
| `.firebaserc` (copy from `.firebaserc.example`) | `"default"` → your project ID |
| `web/firebase-config.js` (copy from `web/firebase-config.example.js`) | `apiKey`, `databaseURL`, `projectId` (`authDomain` is just `<projectId>.firebaseapp.com`) |
| `include/secrets.h` (step 5 below) | `API_KEY`, `DATABASE_URL` |

Easy to miss since none of these will error - the dashboard will just
silently keep talking to whichever project it's pointed at, new or
not.

1. [console.firebase.google.com](https://console.firebase.google.com) → **Add project**
2. **Build → Realtime Database → Create Database** (any region), start
   in locked mode - real rules go in step 3
3. **Build → Authentication → Sign-in method** → enable **Email/Password**
4. Copy the **database URL** and, from Project Settings → General →
   "Your apps" (register a web app if none exists), the **apiKey**
   and **projectId**. `authDomain` doesn't need separately copying -
   it's always `<projectId>.firebaseapp.com` (e.g. a project ID of
   `furrow-123098` means `authDomain: "furrow-123098.firebaseapp.com"`).

## 2. Create the shared device account

This is a dedicated login the *firmware itself* uses - not your own
account, and the same one for every device you ever flash.

Authentication → Users → **Add user** → any email-shaped string works
(doesn't need to be real/deliverable), e.g.
`device@your-project-id.local`, plus a strong password.

## 3. Set RTDB rules

```
cp database.rules.example.json database.rules.json
```

Edit `database.rules.json` and replace every
`device@your-project-id.local` with the real device account email you
just created in step 2 (all four occurrences need to match). Then:

```
firebase deploy --only database
```

This is the recommended path - it keeps your rules in version control
and `firebase deploy --only database` will refuse to run at all if
`database.rules.json` doesn't exist yet, rather than risk deploying
something wrong. If you'd rather paste rules directly into the
Firebase Console instead (Realtime Database → Rules), that works too -
just keep whichever copy you use as the source of truth, since nothing
keeps the two in sync automatically.

```json
{
  "rules": {
    "devices": {
      ".read": true,
      "$deviceId": {
        "status": {
          ".write": "auth.token.email == 'device@your-project-id.local'"
        },
        "motor": {
          ".write": "auth.token.email == 'device@your-project-id.local'"
        },
        "ota": {
          ".write": "auth.token.email == 'device@your-project-id.local'"
        },
        "command": {
          "action": {
            ".write": "auth.token.email == 'device@your-project-id.local' || auth.token.email == root.child('devices').child($deviceId).child('status').child('owner').val()"
          },
          "firmwareUrl": {
            ".write": "auth.token.email == root.child('devices').child($deviceId).child('status').child('owner').val()"
          }
        },
        "displayName": {
          ".write": "auth.token.email == root.child('devices').child($deviceId).child('status').child('owner').val()"
        },
        "fcmTokens": {
          ".write": "auth.token.email == root.child('devices').child($deviceId).child('status').child('owner').val()"
        },
        "autoResume": {
          ".write": "auth.token.email == root.child('devices').child($deviceId).child('status').child('owner').val()"
        },
        "schedule": {
          ".write": "auth.token.email == root.child('devices').child($deviceId).child('status').child('owner').val()"
        }
      }
    }
  }
}
```

Reads stay public (telemetry isn't sensitive); writes are locked to
the device account and whichever owner a specific device declares -
see README.md's Secrets section for why this design.

## 4. Create an owner account (per person using the dashboard)

Authentication → Users → **Add user** → their real email + a password
you set and hand them directly. Repeat per person.

## 5. Set up secrets.h (per machine that builds firmware locally)

```
cp include/secrets.example.h include/secrets.h
```

Fill in the real values from steps 1-2:

```cpp
constexpr const char* API_KEY = "...";
constexpr const char* DATABASE_URL = "...";
constexpr const char* DEVICE_AUTH_EMAIL = "device@your-project-id.local";
constexpr const char* DEVICE_AUTH_PASSWORD = "...";
constexpr const char* WHAPI_TOKEN = "...";  // optional, see step 8
```

`secrets.h` is gitignored - never commit it.

**For CI builds** (GitHub Actions automatically building/publishing on
tag push): add the same values as repository secrets instead -
Settings → Secrets and variables → Actions → New repository secret,
one each for `FIREBASE_API_KEY`, `FIREBASE_DATABASE_URL`,
`DEVICE_AUTH_EMAIL`, `DEVICE_AUTH_PASSWORD`, `WHAPI_TOKEN`. The
workflow generates a real `secrets.h` from these at build time.

**The dashboard needs its own copy of the Firebase values too** - for
local testing, `cp web/firebase-config.example.js web/firebase-config.js`
and fill it in the same way (also gitignored). For the live dashboard,
`.github/workflows/firebase-hosting-deploy.yml` generates it
automatically from the *same* `FIREBASE_API_KEY`/`FIREBASE_DATABASE_URL`
repository secrets above - no separate ones needed.

## 6. Flash the firmware

Two ways to get firmware onto the device - pick whichever fits:

- **Browser flasher (easiest, no local setup)**: on a computer with
  Chrome or Edge, open `flash.html` on your deployed dashboard (e.g.
  `https://furrow-123098.web.app/flash.html`), pick a version, plug
  in the ESP32 over USB, and click through. Pulls the already-built
  firmware straight from this repo's GitHub Releases - nothing to
  install, and none of steps 1-5 above are needed for this path
  specifically, since you're not building anything locally. Doesn't
  work on Safari, Firefox, or any browser on iPhone/iPad - a real
  platform limitation (Web Serial API), not something the page can
  work around.
- **VS Code + PlatformIO** (needed if you're changing the firmware
  itself, not just flashing an existing release): **Build**, then
  **Upload**.

Either way, open Serial Monitor afterward and confirm it boots (no
credentials yet, so it should drop into provisioning mode - see next
step).

## 7. Provision the device

1. On your phone, connect to the WiFi network **`Furrow-Setup-XXXX`**
   the device is broadcasting
2. A setup page should pop up automatically; if not, visit `192.168.4.1`
3. Fill in: your real WiFi network + password, a device name, the
   **owner's email** (must exactly match a Firebase Auth account from
   step 4 - a mismatch silently locks that person out with no error
   shown), and optionally a WhatsApp number for alerts
4. Save - the device restarts and connects to your real network

## 8. WhatsApp alerts (optional)

Set up a [Whapi.cloud](https://whapi.cloud) channel (free sandbox
tier), link a WhatsApp number via QR code, and put the channel token
in `secrets.h`/repository secrets as `WHAPI_TOKEN`. Each device's
*recipient* phone number is set separately, in the provisioning portal
(step 7) - the token is shared, the recipient is per-device.

**Power-loss alerts specifically** (a device going fully silent for
30s+, not just a WiFi hiccup) run as a Firebase Cloud Function -
`functions/index.js`, the `powerWatchdog` scheduled function. It
needs its own deploy step, and **requires your project to be on the
Blaze (pay-as-you-go) pricing plan** - Cloud Functions aren't
available on the free Spark plan at all. See
[firebase.google.com/pricing](https://firebase.google.com/pricing)
before upgrading; realistic usage here should stay within the
no-cost quota, but Blaze still requires linking a real billing
account regardless of what you end up owing.

1. Upgrade to Blaze: Firebase Console → your project → gear icon →
   Usage and billing → Details & settings → **Modify plan**
2. From the repo root, set the Whapi token as a Cloud Functions
   secret (separate from GitHub's secrets - this one needs to exist
   on Google's side instead, since the function runs there):
   ```
   firebase functions:secrets:set WHAPI_TOKEN
   ```
   (paste the same token you used for `secrets.h`/`FIREBASE_API_KEY`
   above when prompted)
3. Install its dependencies (a separate `node_modules` from anything
   else in this repo - `functions/` is its own self-contained project):
   ```
   cd functions
   npm install
   cd ..
   ```
4. Deploy: `firebase deploy --only functions`

Runs on Firebase's own infrastructure, outside any device entirely -
what actually lets it detect a device that's genuinely lost power and
can't report its own absence. Checks every 1 minute (Cloud
Scheduler's granularity, tighter than the ~5 minute practical minimum
of the GitHub-Actions-based version this replaced -
`.github/workflows/power-watchdog.yml` is kept around as a manual-only
diagnostic tool now, not part of the automated path).

## 9. Set up automatic dashboard deploys (optional, one-time)

Without this, you can still deploy manually (`firebase deploy`) - skip
to step 10 if that's fine for now. To have `web/` deploy automatically
whenever it changes on `main`:

1. [console.firebase.google.com](https://console.firebase.google.com)
   → your project → gear icon (top left) → **Project settings**
2. **Service accounts** tab → **Generate new private key** → confirm
   in the popup - a `.json` file downloads automatically

   *(Every Firebase project already has this default admin service
   account created automatically - no need to create a new one by
   hand in Google Cloud Console. That manual route works too, but
   it's an easy way to accidentally create the service account under
   the wrong project if you have more than one - this button can't
   get that wrong, since it only ever acts on the project you're
   already looking at.)*
3. Open that file, copy its entire contents
4. Your GitHub repo → **Settings → Secrets and variables → Actions →
   New repository secret** (not an *environment* secret - a
   different, more restricted scope this workflow can't see)
5. Name: `FIREBASE_SERVICE_ACCOUNT` (exact spelling, case-sensitive) -
   Value: paste the whole JSON - **Add secret**
6. Same place, **New repository secret** again: name
   `FIREBASE_PROJECT_ID`, value is just your project ID exactly as
   shown in the Firebase Console URL (e.g. `furrow-123098`) - this is
   what the workflow deploys *to*, so if you ever switch Firebase
   projects, this is the one thing to update.

Confirm both actually show up under "Repository secrets" in that list
afterward - a secret that silently failed to save is the most common
way this step goes wrong.

## 10. Open the dashboard

If step 9 is done, the dashboard deploys automatically the moment
`web/` changes and gets pushed to `main` - nothing else to do.

Otherwise, deploy it yourself: `firebase deploy` (using the
`firebase.json` already in this repo, plus your local `.firebaserc`
from step 1).

Either way, open the dashboard and sign in with an owner account from
step 4.

## 11. Push notifications (optional)

Browser/PWA push notifications, alongside WhatsApp (step 8), for the
same kinds of events: power loss, power restored, motor starting/
stopping, and a remote command failing to take effect. Also needs the
Blaze plan (same reasoning as step 8, since this runs as Cloud
Functions too) - skip this section if you haven't upgraded and don't
plan to.

1. Firebase Console → your project → gear icon → **Project settings**
   → **General** tab → scroll to **Your apps** → your web app → **SDK
   setup and configuration**. This shows your whole `firebaseConfig`
   object - copy the `appId` and `messagingSenderId` values from it
   (these two aren't used by Auth/Database, which is everything else
   this dashboard needed until now, so they were never set up before -
   skipping them produces a "Missing App configuration value: appId"
   error the moment you try to enable notifications).
2. Same Project settings → **Cloud Messaging** tab → **Web
   configuration** → Web Push certificates → **Generate key pair**.
   Copy the key shown (starts with something like `B...`).
3. Add all three (`appId`, `messagingSenderId`, `vapidKey`) to
   `web/firebase-config.js` (local testing) - see
   `web/firebase-config.example.js` for where each goes - and/or as
   `FIREBASE_APP_ID`, `FIREBASE_MESSAGING_SENDER_ID`, and
   `FIREBASE_VAPID_KEY` repository secrets on GitHub (same place as
   the other `FIREBASE_*` secrets from step 9), so automatic dashboard
   deploys pick them up too.
4. **If you already deployed RTDB rules before this feature existed**,
   redeploy them: `firebase deploy --only database`. The rules gained
   a new `fcmTokens` field (owner-writable, alongside `displayName` -
   see step 3) - without redeploying, the dashboard's write when you
   enable notifications fails with a permissions error, silently as
   far as the UI's concerned beyond the status text.
5. Deploy the new Cloud Functions: `firebase deploy --only functions`
6. Open the dashboard, sign in, select a device, and use the "Push
   notifications" section near the bottom of its panel: **Enable
   notifications** asks your browser for permission, gets a push
   token, and saves it against that specific device. Enable separately
   on each device you want alerts from - it's per-device, not global,
   same as WhatsApp's per-device recipient.

From there it's automatic - no more buttons to press. You'll get a
push when that device loses power, when it recovers, or when its
motor starts or stops (including a physical button press at the
panel, not just remote commands - this comes from the same real
current-sensor feedback everything else uses). The easiest way to
confirm the whole pipeline works after setup is just pressing
Start/Stop from the dashboard and watching for the motor push to
arrive within a few seconds.

---

## 12. Auto-resume after power loss (optional)

If a pump was actually running right before the power went out,
automatically starts it again once the device reconnects - after a
delay you set (1-10 minutes), so it doesn't restart the instant power
returns. Off by default, per device - restores previous state, not an
unconditional "always turn on"; if it was off before the outage, it
stays off. No new deploy needed for the dashboard side, but two
backend pieces need to actually be live first:

1. **If you already deployed RTDB rules before this feature existed**,
   redeploy them: `firebase deploy --only database`. The rules gained
   a new `autoResume` field (owner-writable, alongside `displayName`
   and `fcmTokens` - see step 3) - without redeploying, the dashboard's
   write when you enable this fails with a permissions error, same
   failure mode step 11's own rules note describes.
2. Deploy the new Cloud Functions: `firebase deploy --only functions`.
   This adds a new scheduled function, `autoResumeWatchdog` (runs
   every minute, same as `powerWatchdog`), and extends the existing
   `onPowerRestored` - both need to actually be live for this to do
   anything at all.
3. Open the dashboard, sign in, select a device, and use the
   "Auto-resume after power loss" section in Advanced settings:
   **Enable auto-resume** turns it on for that specific device, and
   the delay input next to it sets how long to wait (defaults to 5
   minutes if left alone). Per-device, same as WhatsApp/push - enabling
   it here doesn't affect any other device.

There's no button to test this the way step 11 has "Send test
notification" - the only real test is an actual power-loss-and-
recovery cycle with the motor running beforehand, which needs real
hardware and can't be simulated from the dashboard. Worth watching
Cloud Functions logs (`autoResumeWatchdog` and `onPowerRestored`) the
first time it's expected to fire, to confirm the whole chain actually
worked rather than just trusting it silently.

---

## 13. Scheduled on/off (optional)

Turns the pump on at a set time every day, and off at another - a
normal daily irrigation schedule. Off by default, per device. Manual
control always wins: stopping it yourself during the "on" window
keeps it off until tomorrow's cycle - the schedule never re-fires the
same on-event twice in one calendar day.

1. **If you already deployed RTDB rules before this feature existed**,
   redeploy them: `firebase deploy --only database`. The rules gained
   a new `schedule` field (owner-writable, same pattern as
   `autoResume` in step 12) - without redeploying, enabling this from
   the dashboard fails with a permissions error.
2. Deploy the new Cloud Functions: `firebase deploy --only functions`.
   This adds `scheduleWatchdog` (runs every minute, same shape as
   `powerWatchdog`/`autoResumeWatchdog`) - needs to actually be live
   for a schedule to do anything.
3. Open the dashboard, select a device, and use the "Scheduled on/off"
   section in Advanced settings (right below auto-resume): set a turn-
   on time and a turn-off time, then **Enable schedule**. Per-device,
   same as everything else in this panel.

Schedule times are interpreted in IST (India Standard Time, UTC+5:30),
fixed - not read from the device's own clock or the browser's
timezone. If devices are ever deployed outside India, this needs to
become configurable rather than assumed.

---

## Troubleshooting

- **`Error: User code failed to load. Cannot determine backend
  specification. Timeout after 10000`** during `firebase deploy
  --only functions` - almost always means `functions/node_modules`
  was never installed, so `require("firebase-functions/...")` fails
  the moment Firebase tries to load and analyze the code, surfacing
  as a generic timeout rather than a clear "module not found." Run
  `npm install` inside `functions/` first (step 8), then redeploy.
- **Dashboard deploy workflow fails with an auth or "no project active"
  error** - almost always one of: `FIREBASE_SERVICE_ACCOUNT` or
  `FIREBASE_PROJECT_ID` missing from Settings → Secrets and variables
  → Actions (must be under "Repository secrets," not "Environment
  secrets" - a different, more restricted scope the workflow can't
  see); or `FIREBASE_PROJECT_ID` doesn't exactly match your project's
  real ID. Double-check the ID against the URL shown in Firebase
  Console itself - a project can end up with a different ID than what
  you typed while creating it, if that exact name was already taken.
- **Dashboard is completely blank / nothing happens, no errors visible
  in the UI** - if you're testing locally (not the deployed version),
  check `web/firebase-config.js` actually exists and is filled in -
  see step 5. Without it, Firebase never initializes at all.
- **"Sign-in failed"** - double-check Email/Password is enabled (step 1)
  and the email/password actually match an Authentication user (step 4).
- **Stuck on "Loading devices..."** - almost always means the page was
  opened directly from disk (`file://`) instead of served over
  `http(s)`. Firebase's SDK is documented to behave unreliably under
  `file://`. Serve it properly (Firebase Hosting, or locally via
  `python -m http.server` for testing) instead.
- **Device shows offline but should be online** - check Serial Monitor
  for WiFi/Firebase connection errors; the dashboard can only be as
  accurate as what the device last successfully published.
- **All buttons greyed out / stuck on "Connecting" forever** - this
  specific symptom happened once from a real process gap: a firmware
  feature was added without bumping `FIRMWARE_VERSION`, so an
  already-flashed device was missing a field the dashboard now
  expected. If you hit something like this, Restart and Update
  Firmware are deliberately *not* gated behind "online" status
  specifically so you can recover via OTA rather than needing a USB
  reflash - try those first.
- **Motor won't stop / Start seems stuck** - motor state is currently
  command-based (assumed), not read from real hardware feedback, until
  the current-sensor-based feedback module lands. See README.md's "Not
  done yet" section.
