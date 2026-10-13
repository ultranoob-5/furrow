// TEMPLATE - for LOCAL testing only. Copy to firebase-config.js (same
// folder) and fill in real values there. firebase-config.js is
// gitignored and never committed.
//
// For actual deployment, this file is generated automatically by
// .github/workflows/firebase-hosting-deploy.yml from GitHub Actions
// repository secrets - never committed there either. See SETUP.md.

const firebaseConfig = {
  apiKey: "REPLACE_WITH_FIREBASE_API_KEY",
  authDomain: "REPLACE_WITH_YOUR_PROJECT_ID.firebaseapp.com", // e.g. "furrow-123098.firebaseapp.com" - just <projectId> + ".firebaseapp.com", not a separate value to look up
  databaseURL: "REPLACE_WITH_FIREBASE_DATABASE_URL",
  projectId: "REPLACE_WITH_FIREBASE_PROJECT_ID",
  // appId and messagingSenderId aren't needed by Auth or the Database
  // (everything this dashboard used until now), so they were never
  // in this file before - but Firebase Cloud Messaging's underlying
  // "Installations" service requires both, or you'll hit "Missing
  // App configuration value: appId" the moment you try to enable
  // push notifications. Both come from the exact same Console page
  // as the VAPID key below: Project Settings -> General tab -> Your
  // apps -> Web app -> SDK setup and configuration (shows the whole
  // config object, not just these two fields).
  appId: "REPLACE_WITH_FIREBASE_APP_ID",
  messagingSenderId: "REPLACE_WITH_FIREBASE_MESSAGING_SENDER_ID",
  // EXPERIMENTAL, optional - only needed for the push-notification
  // proof of concept (see dashboard.html's setupPushExperiment() and
  // SETUP.md). Leave as-is/omit entirely if you're not using that -
  // nothing else on the dashboard depends on it.
  vapidKey: "REPLACE_WITH_FIREBASE_VAPID_KEY",
  // Optional - App Check (reCAPTCHA Enterprise) site key. With it, every
  // Auth/Database/Functions request from the dashboard carries an App
  // Check token. Leave empty to skip App Check. The site key is public
  // (it's sent to every browser), not a secret. See SETUP.md.
  appCheckSiteKey: ""
};
