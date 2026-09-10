// Furrow Service Worker Firebase Modular Entrypoint
// Provides an ultra-compact (~15KB gzipped) background push notification handler
// eliminating the need to load full compat libraries over CDN in sw.js

import { initializeApp } from 'firebase/app';
import { getMessaging, onBackgroundMessage } from 'firebase/messaging/sw';

self.FurrowSwFirebase = {
  init(config, onBackgroundMsgCallback) {
    try {
      const app = initializeApp(config);
      const messaging = getMessaging(app);
      onBackgroundMessage(messaging, onBackgroundMsgCallback);
    } catch (err) {
      console.error('FurrowSwFirebase init failed:', err);
    }
  }
};
