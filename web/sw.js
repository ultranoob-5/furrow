// Minimal service worker: makes the dashboard installable and gives it a
// fast-loading app shell. Deliberately NOT trying to make the dashboard
// work offline for live data - it needs a real connection to Firebase to
// be useful at all. This only caches the static shell (HTML/manifest/
// icons) and always prefers a fresh network copy when online, falling
// back to cache only if the network request actually fails.

const CACHE_NAME = 'furrow-dashboard-v1';

const APP_SHELL = [
  './dashboard.html',
  './manifest.json',
  './icons/icon-192.png',
  './icons/icon-512.png'
];

self.addEventListener('install', (event) => {
  event.waitUntil(
    caches.open(CACHE_NAME).then((cache) => cache.addAll(APP_SHELL))
  );
  self.skipWaiting();
});

self.addEventListener('activate', (event) => {
  event.waitUntil(
    caches.keys().then((keys) =>
      Promise.all(keys.filter((key) => key !== CACHE_NAME).map((key) => caches.delete(key)))
    )
  );
  self.clients.claim();
});

self.addEventListener('fetch', (event) => {
  const url = new URL(event.request.url);

  // Only ever handle same-origin GET requests for the app shell itself -
  // never intercept Firebase's own API/websocket traffic (different
  // origin anyway) or anything else. Live dashboard data always goes
  // straight to the network, completely unaffected by this cache.
  if (event.request.method !== 'GET' || url.origin !== self.location.origin) {
    return;
  }

  event.respondWith(
    fetch(event.request)
      .then((response) => {
        const copy = response.clone();
        caches.open(CACHE_NAME).then((cache) => cache.put(event.request, copy));
        return response;
      })
      .catch(() => caches.match(event.request))
  );
});

// Firebase Cloud Messaging (browser push) background-message handler,
// see dashboard.html's enableNotificationsForSelectedDevice() for the
// enable flow and functions/index.js for what actually triggers a
// send (power loss, motor state changes, power restored). This only
// handles messages that arrive while the dashboard tab isn't in the
// foreground - foreground messages are delivered straight to the page
// instead and are handled there (messaging.onMessage() in
// dashboard.html), not here. Loaded conditionally: if firebase-config.js
// doesn't define a vapidKey, firebase.initializeApp() below still runs
// harmlessly (same public-ish config values already loaded on the page),
// it just never receives anything since no token was ever requested.
try {
  importScripts(
    'https://cdnjs.cloudflare.com/ajax/libs/firebase/12.17.1/firebase-app-compat.min.js',
    'https://cdnjs.cloudflare.com/ajax/libs/firebase/12.17.1/firebase-messaging-compat.min.js',
    'firebase-config.js'
  );

  firebase.initializeApp(firebaseConfig);
  const messaging = firebase.messaging();

  messaging.onBackgroundMessage((payload) => {
    // data, not notification - see functions/index.js's
    // sendPushToDevice() for why (a notification-field message gets
    // auto-displayed by the browser/SDK in addition to this handler's
    // own showNotification() call below, producing a duplicate -
    // confirmed on real hardware, one with the real icon from here,
    // one generic/iconless from the SDK's own fallback display).
    const title = (payload.data && payload.data.title) || 'Furrow';
    const body = (payload.data && payload.data.body) || '';

    self.registration.showNotification(title, {
      body,
      icon: './icons/icon-192.png'
    });
  });
} catch (err) {
  // Non-fatal: the rest of this service worker (app-shell caching,
  // installability) still works fine without push support.
  console.error('FCM background handler setup failed', err);
}
