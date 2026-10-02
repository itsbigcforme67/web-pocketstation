// Offline cache for the app shell (only active on https or localhost).
// Other apps may live on the same site, so this only ever touches caches named wps-*.
const CACHE = 'wps-v3';
const SHELL = ['./', 'index.html', 'style.css', 'app.js', 'core/arm7.js', 'core/pocketstation.js', 'core/memcard.js', 'manifest.webmanifest', 'icon.svg', 'icon-192.png'];
self.addEventListener('install', (e) => e.waitUntil(caches.open(CACHE).then((c) => c.addAll(SHELL)).then(() => self.skipWaiting())));
self.addEventListener('activate', (e) => e.waitUntil(caches.keys().then((ks) => Promise.all(ks.filter((k) => k.startsWith('wps-') && k !== CACHE).map((k) => caches.delete(k)))).then(() => self.clients.claim())));
self.addEventListener('fetch', (e) => {
  const url = new URL(e.request.url);
  if (url.pathname.startsWith('/api/') || e.request.method !== 'GET') return;
  // network first, fall back to cache
  e.respondWith(fetch(e.request).then((r) => { const copy = r.clone(); caches.open(CACHE).then((c) => c.put(e.request, copy)); return r; }).catch(() => caches.match(e.request)));
});
