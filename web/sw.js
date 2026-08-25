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
