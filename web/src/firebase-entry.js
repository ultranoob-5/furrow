// Furrow Modular Firebase v11 Client Entrypoint
// Bundles and tree-shakes only the exact Firebase services required by Furrow:
// - App initialization
// - Authentication (email/password & auth state change)
// - Realtime Database (listeners, writes, updates, server connection status)
// - Cloud Messaging (Web Push token & foreground messaging)
// - Cloud Functions (callable HTTPS endpoints)

import { initializeApp } from 'firebase/app';
import {
  getAuth,
  onAuthStateChanged,
  signInWithEmailAndPassword,
  signOut
} from 'firebase/auth';
import {
  getDatabase,
  ref,
  set,
  get,
  onValue,
  off,
  update,
  remove,
  push,
  child
} from 'firebase/database';
import {
  getMessaging,
  getToken,
  onMessage,
  isSupported
} from 'firebase/messaging';
import {
  getFunctions,
  httpsCallable
} from 'firebase/functions';

let _app = null;
let _auth = null;
let _db = null;
let _messaging = null;
let _functions = null;

// Names of IndexedDB databases used by Firebase that may exist with version > 1
// from older Firebase Compat (e.g. v8) sessions. Firebase v11 expects version 1.
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

export async function purgeLegacyFirebaseDatabases(forceAll = false) {
  if (typeof indexedDB === 'undefined') return;

  if (!forceAll && typeof indexedDB.databases === 'function') {
    try {
      const dbs = await indexedDB.databases();
      for (const db of dbs) {
        if (!db.name) continue;
        const isTarget = LEGACY_FIREBASE_IDB_NAMES.includes(db.name) ||
                         db.name.startsWith('firebase-') ||
                         db.name.includes('fcm');
        // Firebase v11 expects schema version 1. Any version > 1 is a legacy artifact.
        if (isTarget && db.version && db.version > 1) {
          console.warn(`[Furrow] Purging legacy IndexedDB '${db.name}' (v${db.version} > 1) to prevent VersionError...`);
          await deleteDatabasePromise(db.name);
        }
      }
      return;
    } catch (e) {
      console.warn('[Furrow] indexedDB.databases check failed:', e);
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

// Proactively run database cleanup on module initialization
if (typeof window !== 'undefined' && typeof indexedDB !== 'undefined') {
  purgeLegacyFirebaseDatabases(false).catch(() => {});
}

class DatabaseReferenceWrapper {
  constructor(rawRef) {
    this._ref = rawRef;
    this._listeners = new Map();
  }

  child(path) {
    return new DatabaseReferenceWrapper(child(this._ref, path));
  }

  on(eventType, callback, cancelCallback) {
    if (eventType === 'value') {
      const unsubscribe = onValue(
        this._ref,
        (snap) => {
          callback({
            val: () => snap.val(),
            exists: () => snap.exists(),
            key: snap.key
          });
        },
        (err) => {
          if (typeof cancelCallback === 'function') {
            cancelCallback(err);
          } else {
            console.error('Database onValue error:', err);
          }
        }
      );
      this._listeners.set(callback, unsubscribe);
      return callback;
    }
    throw new Error(`Unsupported eventType: ${eventType}`);
  }

  off(eventType, callback) {
    if (callback && this._listeners.has(callback)) {
      const unsub = this._listeners.get(callback);
      unsub();
      this._listeners.delete(callback);
    } else {
      off(this._ref);
      this._listeners.clear();
    }
  }

  async once(eventType) {
    if (eventType === 'value') {
      const snap = await get(this._ref);
      return {
        val: () => snap.val(),
        exists: () => snap.exists(),
        key: snap.key
      };
    }
    throw new Error(`Unsupported eventType: ${eventType}`);
  }

  set(value) {
    return set(this._ref, value);
  }

  update(values) {
    return update(this._ref, values);
  }

  remove() {
    return remove(this._ref);
  }

  push(value) {
    if (value !== undefined) {
      return push(this._ref, value);
    }
    const newRef = push(this._ref);
    return new DatabaseReferenceWrapper(newRef);
  }
}

const FurrowFirebase = {
  initializeApp(config) {
    _app = initializeApp(config);
    _auth = getAuth(_app);
    _db = getDatabase(_app);
    return _app;
  },

  auth() {
    if (!_auth) throw new Error('Firebase Auth not initialized. Call FurrowFirebase.initializeApp() first.');
    return {
      get currentUser() {
        return _auth.currentUser;
      },
      onAuthStateChanged(callback) {
        return onAuthStateChanged(_auth, callback);
      },
      signInWithEmailAndPassword(email, password) {
        return signInWithEmailAndPassword(_auth, email, password);
      },
      signOut() {
        return signOut(_auth);
      }
    };
  },

  database() {
    if (!_db) throw new Error('Firebase Database not initialized. Call FurrowFirebase.initializeApp() first.');
    return {
      ref(path) {
        return new DatabaseReferenceWrapper(path ? ref(_db, path) : ref(_db));
      }
    };
  },

  messaging() {
    return {
      async getToken(options) {
        // Ensure any version > 1 databases from older Firebase versions are purged
        await purgeLegacyFirebaseDatabases(false);

        if (!_messaging) {
          try {
            _messaging = getMessaging(_app);
          } catch (err) {
            console.warn('Firebase Messaging not available:', err);
            throw err;
          }
        }

        try {
          return await getToken(_messaging, options);
        } catch (err) {
          if (isIdbVersionError(err)) {
            console.warn('[Furrow] Encountered IndexedDB VersionError in getToken. Purging legacy databases and retrying...', err);
            await purgeLegacyFirebaseDatabases(true);
            _messaging = getMessaging(_app);
            return await getToken(_messaging, options);
          }
          throw err;
        }
      },
      onMessage(callback) {
        if (!_messaging) {
          try {
            _messaging = getMessaging(_app);
          } catch (err) {
            console.warn('Firebase Messaging not available:', err);
            return () => {};
          }
        }
        return onMessage(_messaging, callback);
      }
    };
  },

  functions() {
    if (!_functions) {
      _functions = getFunctions(_app);
    }
    return {
      httpsCallable(name) {
        const callable = httpsCallable(_functions, name);
        return async (data) => {
          const res = await callable(data);
          return res;
        };
      }
    };
  },

  // Direct modular getters for advanced modular usage
  getApp: () => _app,
  getAuth: () => _auth,
  getDatabase: () => _db,
  getMessaging: () => _messaging,
  purgeLegacyDatabases: purgeLegacyFirebaseDatabases,
  isSupported
};

// Expose globally as FurrowFirebase and alias window.firebase for 100% drop-in compatibility
if (typeof window !== 'undefined') {
  window.FurrowFirebase = FurrowFirebase;
  window.firebase = FurrowFirebase;
}

export default FurrowFirebase;
