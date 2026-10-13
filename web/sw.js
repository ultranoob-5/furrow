// Furrow Service Worker
// Provides an offline-first app shell, intelligent CDN caching for libraries/fonts,
// background push notifications via FCM, and seamless in-app update workflows.

const CACHE_NAME = 'furrow-dashboard-v15';

const APP_SHELL = [
  './dashboard.html',
  './flash.html',
  './manifest.json',
  './firebase-bundle.js',
  './sw-firebase.js',
  './icons/icon-192.png',
  './icons/icon-512.png',
  './icons/apple-touch-icon.png',
  './icons/badge-96.png'
];

// Third-party hosts whose responses can be cached for offline use. Only
// fonts now - ESP Web Tools is self-hosted under vendor/.
const TRUSTED_CDN_HOSTS = [
  'fonts.googleapis.com',
  'fonts.gstatic.com'
];

self.addEventListener('install', (event) => {
  event.waitUntil(
    caches.open(CACHE_NAME).then((cache) => cache.addAll(APP_SHELL))
  );
});

self.addEventListener('activate', (event) => {
  event.waitUntil(
    caches.keys().then((keys) =>
      Promise.all(keys.filter((key) => key !== CACHE_NAME).map((key) => caches.delete(key)))
    )
  );
  self.clients.claim();
});

// Allows the client UI to trigger immediate activation of an updated service worker
self.addEventListener('message', (event) => {
  if (event.data && event.data.type === 'SKIP_WAITING') {
    self.skipWaiting();
  }
});

// Last-resort page when the network is down and nothing is cached yet.
function offlineResponse() {
  return new Response(
    '<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">' +
    '<title>Furrow - offline</title>' +
    '<body style="font-family:system-ui,sans-serif;background:#16181c;color:#e8e8e8;display:flex;align-items:center;justify-content:center;min-height:100vh;margin:0;text-align:center;padding:16px">' +
    '<div><p>You&rsquo;re offline.</p><p><button onclick="location.reload()" style="padding:10px 20px;border-radius:8px;border:0;background:#E8A33D;color:#241804;font-weight:700">Retry</button></p></div>',
    { status: 503, headers: { 'Content-Type': 'text/html; charset=utf-8' } }
  );
}

self.addEventListener('fetch', (event) => {
  if (event.request.method !== 'GET') {
    return;
  }

  const url = new URL(event.request.url);

  // 1. Same-origin assets (app shell, styles, icons, pages)
  if (url.origin === self.location.origin) {
    if (event.request.mode === 'navigate') {
      // Navigation: Network-first, fall back to matching cached page, then dashboard shell
      event.respondWith(
        fetch(event.request)
          .then((response) => {
            const copy = response.clone();
            caches.open(CACHE_NAME).then((cache) => cache.put(event.request, copy));
            return response;
          })
          .catch(() =>
            caches.match(event.request, { ignoreSearch: true })
              .then((matched) => matched || caches.match('./dashboard.html'))
              // respondWith() must always get a Response - undefined here
              // used to surface as "Failed to convert value to 'Response'".
              .then((matched) => matched || offlineResponse())
          )
      );
      return;
    }

    // Static resources: Network-first with cache update and cache fallback
    event.respondWith(
      fetch(event.request)
        .then((response) => {
          if (response && response.status === 200) {
            const copy = response.clone();
            caches.open(CACHE_NAME).then((cache) => cache.put(event.request, copy));
          }
          return response;
        })
        .catch(() => caches.match(event.request).then((matched) => matched || Response.error()))
    );
    return;
  }

  // 2. Trusted CDN libraries and fonts (Firebase SDK, Google Fonts, ESP Web Tools)
  // Stale-While-Revalidate: Return cached version instantly, fetch and update in background
  if (TRUSTED_CDN_HOSTS.includes(url.hostname)) {
    event.respondWith(
      caches.open(CACHE_NAME).then(async (cache) => {
        const cachedResponse = await cache.match(event.request);
        const networkFetch = fetch(event.request)
          .then((networkResponse) => {
            if (networkResponse && (networkResponse.status === 200 || networkResponse.type === 'opaque')) {
              cache.put(event.request, networkResponse.clone());
            }
            return networkResponse;
          })
          .catch(() => null);

        return cachedResponse || networkFetch;
      })
    );
    return;
  }

  // 3. All other requests (Firebase RTDB, Auth APIs, WebSockets) go straight to network
});

// Handles notification clicks: focuses an existing dashboard tab or opens a new one
self.addEventListener('notificationclick', (event) => {
  event.notification.close();

  const rawUrl = (event.notification.data && (event.notification.data.url || event.notification.data.click_action)) || './dashboard.html';
  const targetUrl = new URL(rawUrl, self.location.origin).href;

  event.waitUntil(
    self.clients.matchAll({ type: 'window', includeUncontrolled: true }).then((clientList) => {
      // If a window is already open, focus it and navigate to the target device if needed
      for (const client of clientList) {
        if ('focus' in client) {
          if ('navigate' in client && client.url !== targetUrl) {
            client.navigate(targetUrl);
          }
          return client.focus();
        }
      }
      // Otherwise open a new window/tab
      if (self.clients.openWindow) {
        return self.clients.openWindow(targetUrl);
      }
    })
  );
});

// Firebase Cloud Messaging (browser push) background-message handler
try {
  importScripts('./sw-firebase.js', './firebase-config.js');

  if (self.FurrowSwFirebase && typeof firebaseConfig !== 'undefined') {
    self.FurrowSwFirebase.init(firebaseConfig, (payload) => {
      const title = (payload.data && payload.data.title) || 'Furrow';
      const body = (payload.data && payload.data.body) || '';
      const url = (payload.data && (payload.data.url || payload.data.click_action)) || './dashboard.html';

      const deviceId = payload.data && payload.data.deviceId;
      self.registration.showNotification(title, {
        body,
        icon: './icons/icon-192.png',
        badge: './icons/badge-96.png',
        tag: deviceId ? `furrow-${deviceId}` : 'furrow-alert',
        renotify: true,
        requireInteraction: true,
        vibrate: [200, 100, 200],
        data: { url, deviceId }
      });
    });
  }
} catch (err) {
  // Non-fatal: app-shell caching and installability continue working without push support
  console.error('FCM background handler setup failed', err);
}
