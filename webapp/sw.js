// Service worker minimal - existe surtout pour satisfaire le critere
// d'installabilite de Chrome sur Android (icone + lancement plein ecran
// sans barre d'adresse). Pas de vrai cache offline: cette page ne sert a
// rien sans reseau (elle parle uniquement au cloud Particle), donc on se
// contente de laisser passer chaque requete vers le reseau.
self.addEventListener('install', (event) => {
  self.skipWaiting();
});

self.addEventListener('activate', (event) => {
  self.clients.claim();
});

self.addEventListener('fetch', (event) => {
  event.respondWith(fetch(event.request));
});
