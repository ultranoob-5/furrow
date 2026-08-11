#pragma once

// TEMPLATE - copy this file to secrets.h (same folder) and fill in real
// values there. secrets.h is gitignored and never committed - this
// example file is the only one that lives in the repo.
//
// For CI builds, these same values come from GitHub Actions repository
// secrets instead (Settings > Secrets and variables > Actions on your
// repo) - the build workflow generates a real secrets.h from those at
// build time. See SETUP.md for exact steps either way.

namespace Config
{
    // Firebase project details (from Firebase Console > Project
    // Settings > General, and Realtime Database)
    constexpr const char* API_KEY = "REPLACE_WITH_FIREBASE_API_KEY";
    constexpr const char* DATABASE_URL = "REPLACE_WITH_FIREBASE_DATABASE_URL";

    // Dedicated Firebase Auth account for the firmware itself (Email/
    // Password provider) - shared across every device, since its only
    // job is proving "this write came from legitimate firmware," not
    // identifying which specific device or owner. NOT a friend's own
    // login. Create this once in Firebase Console > Authentication >
    // Users > Add user.
    constexpr const char* DEVICE_AUTH_EMAIL = "REPLACE_WITH_DEVICE_ACCOUNT_EMAIL";
    constexpr const char* DEVICE_AUTH_PASSWORD = "REPLACE_WITH_DEVICE_ACCOUNT_PASSWORD";

    // WhatsApp alerts, via Whapi.cloud (https://whapi.cloud). One
    // shared sender channel/number for every device - this token
    // authorizes sending FROM that number, so it's project-wide like
    // the Firebase credentials above, not per-device. The RECIPIENT
    // phone number is still per-device, set via the captive portal.
    constexpr const char* WHAPI_TOKEN = "REPLACE_WITH_WHAPI_CHANNEL_TOKEN";
}
