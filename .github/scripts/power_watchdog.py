#!/usr/bin/env python3
"""
Power-loss watchdog. Runs on a schedule (see
.github/workflows/power-watchdog.yml), outside any device entirely -
this is what makes it able to detect a device that's genuinely lost
power and can't report its own absence.

For every device: if it's been silent (lastSeen) for 30s or longer
and hasn't already been alerted for this specific outage, sends a
WhatsApp message to that device's configured recipient and marks it
alerted (so it doesn't get re-sent every run for as long as the
outage continues). The device itself clears that flag automatically
the moment it's back and reporting again - see
Cloud::publishDevice() in src/cloud.cpp.

Detection latency is bounded by how often this workflow runs (see the
cron schedule), not by the 30s threshold itself - a device that's
been down for 45s might not get flagged until several minutes later,
whenever the next scheduled run happens to land.
"""

import os
import sys
import time
import urllib.request
import json

DATABASE_URL = os.environ["FIREBASE_DATABASE_URL"].rstrip("/")
API_KEY = os.environ["FIREBASE_API_KEY"]
DEVICE_AUTH_EMAIL = os.environ["DEVICE_AUTH_EMAIL"]
DEVICE_AUTH_PASSWORD = os.environ["DEVICE_AUTH_PASSWORD"]
WHAPI_TOKEN = os.environ["WHAPI_TOKEN"]

OFFLINE_THRESHOLD_MS = 30000


def http_json(url, data=None, method="GET", headers=None):
    headers = headers or {}
    body = json.dumps(data).encode("utf-8") if data is not None else None
    if body is not None:
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=body, method=method, headers=headers)
    with urllib.request.urlopen(req, timeout=15) as resp:
        raw = resp.read()
        return json.loads(raw) if raw else None


def get_device_auth_token():
    # Signs in as the same device account the firmware itself uses -
    # already has write access to status/* per the RTDB rules (see
    # SETUP.md), so no new rules or secrets needed for this.
    url = f"https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key={API_KEY}"
    result = http_json(url, {
        "email": DEVICE_AUTH_EMAIL,
        "password": DEVICE_AUTH_PASSWORD,
        "returnSecureToken": True,
    }, method="POST")
    return result["idToken"]


def send_whatsapp(phone, message):
    req = urllib.request.Request(
        "https://gate.whapi.cloud/messages/text",
        data=json.dumps({"to": phone, "body": message}).encode("utf-8"),
        method="POST",
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {WHAPI_TOKEN}",
        },
    )
    with urllib.request.urlopen(req, timeout=15) as resp:
        return resp.status


def main():
    devices = http_json(f"{DATABASE_URL}/devices.json") or {}

    now_ms = int(time.time() * 1000)
    id_token = None  # only fetched if we actually need to write something

    for device_id, data in devices.items():
        status = (data or {}).get("status") or {}
        last_seen = status.get("lastSeen")
        name = status.get("name", device_id)
        phone = status.get("whatsappPhone")
        already_alerted = status.get("powerAlertSent", False)

        if last_seen is None:
            continue  # never reported at all - nothing to compare against

        age_ms = now_ms - last_seen

        if age_ms < OFFLINE_THRESHOLD_MS:
            continue  # still within normal heartbeat range

        if already_alerted:
            continue  # already sent for this outage, don't spam every run

        if not phone:
            print(f"{device_id} ({name}): offline {age_ms // 1000}s, no WhatsApp recipient configured - skipping")
            continue

        print(f"{device_id} ({name}): offline {age_ms // 1000}s - alerting {phone}")

        try:
            send_whatsapp(phone, f"\u26a0\ufe0f {name} appears to have lost power - no contact for over 30s")
        except Exception as err:
            print(f"  WhatsApp send failed: {err}", file=sys.stderr)
            continue  # don't mark alerted if the send itself failed

        if id_token is None:
            id_token = get_device_auth_token()

        try:
            http_json(
                f"{DATABASE_URL}/devices/{device_id}/status/powerAlertSent.json?auth={id_token}",
                data=True,
                method="PUT",
            )
        except Exception as err:
            print(f"  Failed to set dedup flag (may re-alert next run): {err}", file=sys.stderr)


if __name__ == "__main__":
    main()
