// Furrow Service Worker Firebase Modular Entrypoint
// Provides an ultra-compact (~15KB gzipped) background push notification handler
// eliminating the need to load full compat libraries over CDN in sw.js

import { initializeApp } from 'firebase/app';
import { getMessaging, onBackgroundMessage } from 'firebase/messaging/sw';

const LEGACY_FIREBASE_IDB_NAMES = [
  'firebase-messaging-database',
  'firebase-installations-database',
  'firebase-heartbeat-database',
  'fcm_token_details_db',
  'fcm_vapid_details_db'
];

function deleteDatabasePromise(name) {
  return new Promise((resolve) => {
    if (typeof indexedDB === 'undefined' || !indexedDB.deleteDatabase) return resolve();
    try {
      const req = indexedDB.deleteDatabase(name);
      req.onsuccess = () => resolve();
      req.onerror = () => resolve();
      req.onblocked = () => resolve();
      setTimeout(resolve, 500);
    } catch (_) {
      resolve();
    }
  });
}

async function purgeLegacyFirebaseDatabases(forceAll = false) {
  if (typeof indexedDB === 'undefined') return;

  if (!forceAll && typeof indexedDB.databases === 'function') {
    try {
      const dbs = await indexedDB.databases();
      for (const db of dbs) {
        if (!db.name) continue;
        const isTarget = LEGACY_FIREBASE_IDB_NAMES.includes(db.name) ||
                         db.name.startsWith('firebase-') ||
                         db.name.includes('fcm');
        if (isTarget && db.version && db.version > 1) {
          console.warn(`[Furrow SW] Purging legacy IndexedDB '${db.name}' (v${db.version} > 1)...`);
          await deleteDatabasePromise(db.name);
        }
      }
      return;
    } catch (e) {
      console.warn('[Furrow SW] indexedDB.databases check failed:', e);
    }
  }

  if (forceAll) {
    for (const name of LEGACY_FIREBASE_IDB_NAMES) {
      await deleteDatabasePromise(name);
    }
  }
}

function isIdbVersionError(err) {
  if (!err) return false;
  const name = err.name || '';
  const msg = err.message || '';
  return name === 'VersionError' ||
         msg.includes('VersionError') ||
         msg.includes('less than the existing version') ||
         msg.includes('requested version');
}

// Proactive purge on Service Worker start
if (typeof indexedDB !== 'undefined') {
  purgeLegacyFirebaseDatabases(false).catch(() => {});
}

self.FurrowSwFirebase = {
  async init(config, onBackgroundMsgCallback) {
    try {
      await purgeLegacyFirebaseDatabases(false);
      const app = initializeApp(config);
      const messaging = getMessaging(app);
      onBackgroundMessage(messaging, onBackgroundMsgCallback);
    } catch (err) {
      if (isIdbVersionError(err)) {
        console.warn('[Furrow SW] Recovering from IndexedDB VersionError in SW init...');
        await purgeLegacyFirebaseDatabases(true);
        try {
          const app = initializeApp(config);
          const messaging = getMessaging(app);
          onBackgroundMessage(messaging, onBackgroundMsgCallback);
          return;
        } catch (retryErr) {
          console.error('[Furrow SW] Retry init failed:', retryErr);
        }
      }
      console.error('FurrowSwFirebase init failed:', err);
    }
  }
};
