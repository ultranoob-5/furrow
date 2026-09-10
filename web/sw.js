// Furrow Service Worker
// Provides an offline-first app shell, intelligent CDN caching for libraries/fonts,
// background push notifications via FCM, and seamless in-app update workflows.

const CACHE_NAME = 'furrow-dashboard-v4';

const APP_SHELL = [
  './dashboard.html',
  './flash.html',
  './manifest.json',
  './firebase-bundle.js',
  './sw-firebase.js',
  './icons/icon-192.png',
  './icons/icon-512.png',
  './icons/apple-touch-icon.png'
];

// Pinned third-party CDNs used by Furrow that can be safely cached for offline reliability
const TRUSTED_CDN_HOSTS = [
  'fonts.googleapis.com',
  'fonts.gstatic.com',
  'unpkg.com'
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
            caches.match(event.request).then((matched) => matched || caches.match('./dashboard.html'))
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
        .catch(() => caches.match(event.request))
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

      self.registration.showNotification(title, {
        body,
        icon: './icons/icon-192.png',
        badge: './icons/icon-192.png',
        data: { url }
      });
    });
  }
} catch (err) {
  // Non-fatal: app-shell caching and installability continue working without push support
  console.error('FCM background handler setup failed', err);
}
