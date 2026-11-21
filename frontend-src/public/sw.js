// EventSphere service worker: makes the app installable and lets the app shell open without a connection.
// API responses are NEVER cached here (they are personal data); offline scans are queued by the page itself (src/offlineQueue.js).
const V='eventsphere-v1',SHELL=['/','/index.html','/manifest.webmanifest','/icons/icon-192.png'];
self.addEventListener('install',e=>{e.waitUntil(caches.open(V).then(c=>c.addAll(SHELL)).then(()=>self.skipWaiting()))});
self.addEventListener('activate',e=>{e.waitUntil(caches.keys().then(ks=>Promise.all(ks.filter(k=>k!==V).map(k=>caches.delete(k)))).then(()=>self.clients.claim()))});
self.addEventListener('fetch',e=>{const r=e.request,u=new URL(r.url);
 if(r.method!=='GET'||u.origin!==location.origin||u.pathname.startsWith('/api/')||u.pathname.startsWith('/docs'))return;
 if(r.mode==='navigate'){e.respondWith(fetch(r).then(res=>{const cp=res.clone();caches.open(V).then(c=>c.put('/index.html',cp));return res}).catch(()=>caches.match('/index.html')));return}
 e.respondWith(caches.match(r).then(hit=>hit||fetch(r).then(res=>{if(res.ok&&(u.pathname.startsWith('/assets/')||u.pathname.startsWith('/icons/'))){const cp=res.clone();caches.open(V).then(c=>c.put(r,cp))}return res})))});
