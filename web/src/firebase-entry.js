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
  child,
  query,
  orderByChild,
  orderByKey,
  orderByValue,
  equalTo,
  limitToFirst,
  limitToLast,
  startAt,
  endAt
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

class DatabaseQueryWrapper {
  constructor(rawRef) {
    this._ref = rawRef;
    this._listeners = new Map();
  }

  get key() {
    return this._ref ? this._ref.key : null;
  }

  orderByChild(path) {
    return new DatabaseQueryWrapper(query(this._ref, orderByChild(path)));
  }

  orderByKey() {
    return new DatabaseQueryWrapper(query(this._ref, orderByKey()));
  }

  orderByValue() {
    return new DatabaseQueryWrapper(query(this._ref, orderByValue()));
  }

  equalTo(value, key) {
    return new DatabaseQueryWrapper(
      key !== undefined ? query(this._ref, equalTo(value, key)) : query(this._ref, equalTo(value))
    );
  }

  limitToFirst(limit) {
    return new DatabaseQueryWrapper(query(this._ref, limitToFirst(limit)));
  }

  limitToLast(limit) {
    return new DatabaseQueryWrapper(query(this._ref, limitToLast(limit)));
  }

  startAt(value, key) {
    return new DatabaseQueryWrapper(
      key !== undefined ? query(this._ref, startAt(value, key)) : query(this._ref, startAt(value))
    );
  }

  endAt(value, key) {
    return new DatabaseQueryWrapper(
      key !== undefined ? query(this._ref, endAt(value, key)) : query(this._ref, endAt(value))
    );
  }

  on(eventType, callback, cancelCallback) {
    if (eventType === 'value') {
      const unsubscribe = onValue(
        this._ref,
        (snap) => {
          callback({
            val: () => snap.val(),
            exists: () => snap.exists(),
            key: snap.key,
            forEach: (cb) => {
              snap.forEach((childSnap) => {
                cb({
                  val: () => childSnap.val(),
                  exists: () => childSnap.exists(),
                  key: childSnap.key
                });
              });
            }
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
      for (const unsub of this._listeners.values()) {
        try { unsub(); } catch (_) {}
      }
      this._listeners.clear();
      try {
        off(this._ref);
      } catch (_) {}
    }
  }

  async once(eventType) {
    if (eventType === 'value') {
      const snap = await get(this._ref);
      return {
        val: () => snap.val(),
        exists: () => snap.exists(),
        key: snap.key,
        forEach: (cb) => {
          snap.forEach((childSnap) => {
            cb({
              val: () => childSnap.val(),
              exists: () => childSnap.exists(),
              key: childSnap.key
            });
          });
        }
      };
    }
    throw new Error(`Unsupported eventType: ${eventType}`);
  }
}

class DatabaseReferenceWrapper extends DatabaseQueryWrapper {
  constructor(rawRef) {
    super(rawRef);
  }

  child(path) {
    return new DatabaseReferenceWrapper(child(this._ref, path));
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
    if (!_messaging) {
      try {
        _messaging = getMessaging(_app);
      } catch (err) {
        console.warn('Firebase Messaging not available:', err);
      }
    }
    return {
      async getToken(options) {
        if (!_messaging) throw new Error('Messaging not initialized');
        return getToken(_messaging, options);
      },
      onMessage(callback) {
        if (!_messaging) return () => {};
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
  isSupported
};

// Expose globally as FurrowFirebase and alias window.firebase for 100% drop-in compatibility
if (typeof window !== 'undefined') {
  window.FurrowFirebase = FurrowFirebase;
  window.firebase = FurrowFirebase;
}

export default FurrowFirebase;
